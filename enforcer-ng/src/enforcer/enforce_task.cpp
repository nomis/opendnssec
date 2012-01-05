#include <ctime>
#include <iostream>
#include <cassert>
#include <fcntl.h>
#include <map>

#include "policy/kasp.pb.h"
#include "zone/zonelist.pb.h"
#include "keystate/keystate.pb.h"

#include "enforcer/enforcerdata.h"
#include "enforcer/enforcer.h"

#include "daemon/engine.h"
#include "daemon/orm.h"
#include "enforcer/enforce_task.h"
#include "hsmkey/hsmkey_gen_task.h"
#include "signconf/signconf_task.h"
#include "keystate/keystate_ds_submit_task.h"
#include "keystate/keystate_ds_retract_task.h"
#include "shared/duration.h"
#include "shared/file.h"
#include "shared/allocator.h"

#include "enforcer/enforcerzone.h"
#include "enforcer/hsmkeyfactory.h"

#include "protobuf-orm/pb-orm.h"

static const char *module_str = "enforce_task";

static void 
schedule_task(int sockfd, engine_type* engine, task_type *task, const char *what)
{
    /* schedule task */
    if (!task) {
        ods_log_crit("[%s] failed to create %s task", module_str, what);
    } else {
        char buf[ODS_SE_MAXLINE];
        ods_status status = lock_and_schedule_task(engine->taskq, task, 0);
        if (status != ODS_STATUS_OK) {
            ods_log_crit("[%s] failed to create %s task", module_str, what);
            (void)snprintf(buf, ODS_SE_MAXLINE,
                           "Unable to schedule %s task.\n", what);
            ods_writen(sockfd, buf, strlen(buf));
        } else {
            (void)snprintf(buf, ODS_SE_MAXLINE,
                           "Scheduled %s task.\n", what);
            ods_writen(sockfd, buf, strlen(buf));
            engine_wakeup_workers(engine);
        }
    }
}

class HsmKeyFactoryCallbacks : public HsmKeyFactoryDelegatePB {
private:
    int _sockfd;
    engine_type *_engine;
    bool _bShouldLaunchKeyGen;
public:
    
    HsmKeyFactoryCallbacks(int sockfd, engine_type *engine)
    : _sockfd(sockfd),_engine(engine), _bShouldLaunchKeyGen(false)
    {
        
    }
    
    ~HsmKeyFactoryCallbacks()
    {
        if (_bShouldLaunchKeyGen) {
			// Keys were given out by the key factory during the last enforce.
			// We need to schedule the "hsm key gen" task to create additional
			// keys if needed.
			schedule_task(_sockfd, _engine,hsmkey_gen_task(_engine->config),
						  "hsm key gen");
		}
    }

    virtual void OnKeyCreated(int bits, const std::string &repository,
                              const std::string &policy, int algorithm,
                              KeyRole role)
    {
        _bShouldLaunchKeyGen = true;
    }
    
    virtual void OnKeyShortage(int bits, const std::string &repository,
                               const std::string &policy, int algorithm,
                               KeyRole role)
    {
        _bShouldLaunchKeyGen = true;
    }
};

static bool
load_kasp_policy(OrmConn conn,const std::string &name,
				 ::ods::kasp::Policy &policy)
{
	std::string qname;
	if (!OrmQuoteStringValue(conn, name, qname))
		return false;
	
	OrmResultRef rows;
	if (!OrmMessageEnumWhere(conn,policy.descriptor(),rows,
							 "name=%s",qname.c_str()))
		return false;
	
	if (!OrmFirst(rows))
		return false;
	
	return OrmGetMessage(rows, policy, true);
}

static time_t 
reschedule_enforce(task_type *task, time_t t_when, const char *z_when)
{
    if (!task)
        return -1;
    
    ods_log_assert(task->allocator);
    ods_log_assert(task->who);
    allocator_deallocate(task->allocator,(void*)task->who);
    task->who = allocator_strdup(task->allocator, z_when);

    task->when = std::max(t_when, time_now());
    task->backoff = 0;
    return task->when;
}

time_t perform_enforce(int sockfd, engine_type *engine, int bForceUpdate,
                       task_type* task)
{
	#define LOG_AND_RESCHEDULE(errmsg)\
		do {\
			ods_log_error_and_printf(sockfd,module_str,errmsg);\
			ods_log_error("[%s] retrying in 30 minutes", module_str);\
			return reschedule_enforce(task,t_now + 30*60, "next zone");\
		} while (0)

	#define LOG_AND_RESCHEDULE_15SECS(errmsg)\
		do {\
			ods_log_error_and_printf(sockfd,module_str,errmsg);\
			ods_log_error("[%s] retrying in 15 seconds", module_str);\
			return reschedule_enforce(task,t_now + 15, "next zone");\
		} while (0)
	
	#define LOG_AND_RESCHEDULE_1(errmsg,param)\
		do {\
			ods_log_error_and_printf(sockfd,module_str,errmsg,param);\
			ods_log_error("[%s] retrying in 30 minutes", module_str);\
			return reschedule_enforce(task,t_now + 30*60, "next zone");\
		} while (0)
	
	GOOGLE_PROTOBUF_VERIFY_VERSION;
	
    time_t t_now = time_now();

	OrmConnRef conn;
	if (!ods_orm_connect(sockfd, engine->config, conn)) {
		ods_log_error("[%s] retrying in 30 minutes", module_str);
		return reschedule_enforce(task,t_now + 30*60, "next zone");
	}

	std::auto_ptr< HsmKeyFactoryCallbacks > hsmKeyFactoryCallbacks(
									new HsmKeyFactoryCallbacks(sockfd,engine));
    // Hook the key factory up with the database
    HsmKeyFactoryPB keyfactory(conn,hsmKeyFactoryCallbacks.get());

	// Flags that indicate tasks to be scheduled after zones have been enforced.
    bool bSignerConfNeedsWriting = false;
    bool bSubmitToParent = false;
    bool bRetractFromParent = false;

	{	OrmTransactionRW transaction(conn);
		if (!transaction.started())
			LOG_AND_RESCHEDULE_15SECS("transaction not started");
		
		{	OrmResultRef rows;
			::ods::keystate::EnforcerZone enfzone;
			
			bool ok;
			if (bForceUpdate)
				ok = OrmMessageEnum(conn,enfzone.descriptor(),rows);
			else {
				const char *where = "next_change IS NULL OR next_change <= %d";
				ok = OrmMessageEnumWhere(conn,enfzone.descriptor(),rows,where,t_now);
			}
			if (!ok)
				LOG_AND_RESCHEDULE_15SECS("zone enumeration failed");
			
			// Go through all the zones that need handling and call enforcer
			// update for the zone when its schedule time is earlier or
			// identical to time_now.
			int batchcountdown = 5; // enforce 5 zones per transaction
			for (bool next=OrmFirst(rows); next && batchcountdown; next = OrmNext(rows)) {

				OrmContextRef context;
				if (!OrmGetMessage(rows, enfzone, /*zones + keys*/true, context))
					LOG_AND_RESCHEDULE_15SECS("retrieving zone from database failed");

				::ods::kasp::Policy policy;
				if (!load_kasp_policy(conn, enfzone.policy(), policy)) {

					// Policy for this zone not found, so don't schedule
					// it again !
					ods_printf(sockfd,
							   "Next update for zone %s NOT scheduled "
							   "because policy %s is missing !\n",
							   enfzone.name().c_str(),
							   enfzone.policy().c_str());
					enfzone.set_next_change((time_t)-1);
					
				} else {
					
					EnforcerZonePB enfZone(&enfzone, policy);
					time_t t_next = update(enfZone, time_now(), keyfactory);
					
					if (enfZone.signerConfNeedsWriting())
						bSignerConfNeedsWriting = true;
					
					KeyDataList &kdl = enfZone.keyDataList();
					for (int k=0; k<kdl.numKeys(); ++k) {
						if (kdl.key(k).dsAtParent() == DS_SUBMIT)
							bSubmitToParent = true;
						if (kdl.key(k).dsAtParent() == DS_RETRACT)
							bRetractFromParent = true;
					}
					
					if (t_next == -1) {
						ods_printf(sockfd,
								   "Next update for zone %s NOT scheduled "
								   "by enforcer !\n",
									enfzone.name().c_str());
					}
						
					enfZone.setNextChange(t_next);
					if (t_next != -1) {
						// Invalid schedule time then skip the zone.
						char tbuf[32] = "date/time invalid\n"; // at least 26 bytes
						ctime_r(&t_next,tbuf); // note that ctime_r inserts a \n
						ods_printf(sockfd,
								   "Next update for zone %s scheduled at %s",
								   enfzone.name().c_str(),
								   tbuf);
					}

				}

				if (!OrmMessageUpdate(context))
					LOG_AND_RESCHEDULE_15SECS("updating zone in the database failed");
				
				// Perform a limited number of zone updates per transaction.
				--batchcountdown;
			}
			
			// we no longer need the query result, so release it.
			rows.release();
			
			if (!transaction.commit())
				LOG_AND_RESCHEDULE_15SECS("committing updated zones to the database failed");
		}
	}

    // Delete the call backs and launch key pre-generation when we ran out 
    // of keys during the enforcement
    hsmKeyFactoryCallbacks.reset();


	// when to reschedule next zone for enforcement
    time_t t_when = t_now + 1 * 365 * 24 * 60 * 60; // now + 1 year
    // which zone to reschedule next for enforcement
    std::string z_when("next zone");

	{	OrmTransaction transaction(conn);
		if (!transaction.started())
			LOG_AND_RESCHEDULE_15SECS("transaction not started");
		
		{	OrmResultRef rows;
			::ods::keystate::EnforcerZone enfzone;
			
			// Determine the next schedule time.
			const char *where =
				"next_change IS NULL OR next_change > 0 ORDER BY next_change";
			if (!OrmMessageEnumWhere(conn,enfzone.descriptor(),rows,where))
				LOG_AND_RESCHEDULE_15SECS("zone query failed");
			
			if (!OrmFirst(rows))
				LOG_AND_RESCHEDULE("unable to determine next schedule time");
			
			if (!OrmGetMessage(rows, enfzone, false))
				LOG_AND_RESCHEDULE("unable to retriev zone from database");
			
			// t_next can never go negative as next_change is a uint32 and 
			// time_t is a long (int64) so -1 stored in next_change will 
			// become maxint in t_next.
			time_t t_next = enfzone.next_change();
			
			// Determine whether this zone is going to be scheduled next.
			// If the enforcer wants a reschedule earlier than currently
			// set, then use that.
			if (t_next < t_when) {
				t_when = t_next;
				z_when = enfzone.name().c_str();
			}
		}
	}

    // Launch signer configuration writer task when one of the 
    // zones indicated that it needs to be written.
    if (bSignerConfNeedsWriting) {
        task_type *signconf =
            signconf_task(engine->config, "signconf", "signer configurations");
        schedule_task(sockfd,engine,signconf,"signconf");
    }

    // Launch ds-submit task when one of the updated key states has the
    // DS_SUBMIT flag set.
    if (bSubmitToParent) {
        task_type *submit =
            keystate_ds_submit_task(engine->config,
                                    "ds-submit","KSK keys with submit flag set");
        schedule_task(sockfd,engine,submit,"ds-submit");
    }

    // Launch ds-retract task when one of the updated key states has the
    // DS_RETRACT flag set.
    if (bRetractFromParent) {
        task_type *retract =
            keystate_ds_retract_task(engine->config,
                                "ds-retract","KSK keys with retract flag set");
        schedule_task(sockfd,engine,retract,"ds-retract");
    }

    return reschedule_enforce(task,t_when,z_when.c_str());
}

static task_type *
enforce_task_perform(task_type *task)
{
    if (perform_enforce(-1, (engine_type *)task->context, 0, task) != -1)
        return task;

    task_cleanup(task);
    return NULL;
}

task_type *
enforce_task(engine_type *engine)
{
    const char *what = "enforce";
    const char *who = "next zone";
    task_id what_id = task_register(what, 
                                 "enforce_task_perform",
                                 enforce_task_perform);
    return task_create(what_id, time_now(), who, (void*)engine);
}


int
flush_enforce_task(engine_type *engine)
{
    /* flush (force to run) the enforcer task when it is waiting in the 
     task list. */
    task_type *enf = enforce_task(engine);
    lock_basic_lock(&engine->taskq->schedule_lock);
    /* [LOCK] schedule */
    task_type *running_enforcer = schedule_lookup_task(engine->taskq, enf);
    task_cleanup(enf);
    if (running_enforcer)
        running_enforcer->flush = 1;
    /* [UNLOCK] schedule */
    lock_basic_unlock(&engine->taskq->schedule_lock);
    if (running_enforcer)
        engine_wakeup_workers(engine);
    return running_enforcer ? 1 : 0;
}
