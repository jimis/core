/*
   Copyright (C) Cfengine AS

   This file is part of Cfengine 3 - written and maintained by Cfengine AS.

   This program is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the
   Free Software Foundation; version 3.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA

  To the extent this program is licensed as part of the Enterprise
  versions of Cfengine, the applicable Commerical Open Source License
  (COSL) may apply to this file if you as a licensee so wish it. See
  included file COSL.txt.

*/

/*****************************************************************************/
/*                                                                           */
/* File: transaction.c                                                       */
/*                                                                           */
/*****************************************************************************/

#include "cf3.defs.h"
#include "cf3.extern.h"

#include "transaction.h"
#include "dbm_api.h"
#include "files_names.h"

static void WaitForCriticalSection(void);
static void ReleaseCriticalSection(void);
static time_t FindLock(char *last);
static int RemoveLock(char *name);
static void LogLockCompletion(char *cflog, int pid, char *str, char *operator, char *operand);
static pid_t FindLockPid(char *name);
static void RemoveDates(char *s);
static bool WriteLockDataCurrent(CF_DB *dbp, char *lock_id);
static bool WriteLockData(CF_DB *dbp, char *lock_id, LockData *lock_data);


/*****************************************************************************/

void SummarizeTransaction(Attributes attr, Promise *pp, char *logname)
{
    if (logname && attr.transaction.log_string)
    {
        char buffer[CF_EXPANDSIZE];

        ExpandPrivateScalar(CONTEXTID, attr.transaction.log_string, buffer);

        if (strcmp(logname, "udp_syslog") == 0)
        {
            RemoteSysLog(attr.transaction.log_priority, buffer);
        }
        else if (strcmp(logname, "stdout") == 0)
        {
            CfOut(cf_reporting, "", "L: %s\n", buffer);
        }
        else
        {
            FILE *fout = fopen(logname, "a");

            if (fout == NULL)
            {
                CfOut(cf_error, "", "Unable to open private log %s", logname);
                return;
            }

            CfOut(cf_verbose, "", " -> Logging string \"%s\" to %s\n", buffer, logname);
            fprintf(fout, "%s\n", buffer);

            fclose(fout);
        }

        attr.transaction.log_string = NULL;     /* To avoid repetition */
    }
    else if (attr.transaction.log_failed)
    {
        if (logname && strcmp(logname, attr.transaction.log_failed) == 0)
        {
            cfPS(cf_log, CF_NOP, "", pp, attr, "%s", attr.transaction.log_string);
        }
    }
}

/*****************************************************************************/

#ifdef __MINGW32__

static bool KillLockHolder(ARG_UNUSED const char *lock)
{
    CfOut(cf_verbose, "",
          "Process with pid %d is not running - ignoring lock (Windows does not support graceful processes termination)\n",
          pid);
    return true;
}

#else

static bool KillLockHolder(const char *lock)
{
    CF_DB *dbp = OpenLock();
    if (dbp == NULL)
    {
        CfOut(cf_error, "", "Unable to open locks database");
        return false;
    }

    LockData lock_data;
    lock_data.process_start_time = PROCESS_START_TIME_UNKNOWN;


    if (!ReadDB(dbp, lock, &lock_data, sizeof(lock_data)))
    {
        /* No lock found */
        CloseLock(dbp);
        return true;
    }

    CloseLock(dbp);

    return GracefulTerminate(lock_data.pid, lock_data.process_start_time);
}

#endif

CfLock AcquireLock(char *operand, char *host, time_t now, Attributes attr, Promise *pp, int ignoreProcesses)
{
    int i, sum = 0;
    time_t lastcompleted = 0, elapsedtime;
    char *promise, cc_operator[CF_BUFSIZE], cc_operand[CF_BUFSIZE];
    char cflock[CF_BUFSIZE], cflast[CF_BUFSIZE], cflog[CF_BUFSIZE];
    char str_digest[CF_BUFSIZE];
    CfLock this;
    unsigned char digest[EVP_MAX_MD_SIZE + 1];

    this.last = (char *) CF_UNDEFINED;
    this.lock = (char *) CF_UNDEFINED;
    this.log = (char *) CF_UNDEFINED;

    if (now == 0)
    {
        return this;
    }

    this.last = NULL;
    this.lock = NULL;
    this.log = NULL;

/* Indicate as done if we tried ... as we have passed all class
   constraints now but we should only do this for level 0
   promises. Sub routine bundles cannot be marked as done or it will
   disallow iteration over bundles */

    if (pp->done)
    {
        return this;
    }

    if (CF_STCKFRAME == 1)
    {
        *(pp->donep) = true;
        /* Must not set pp->done = true for editfiles etc */
    }

    HashPromise(operand, pp, digest, CF_DEFAULT_DIGEST);
    strcpy(str_digest, HashPrint(CF_DEFAULT_DIGEST, digest));

/* As a backup to "done" we need something immune to re-use */

    if (THIS_AGENT_TYPE == cf_agent)
    {
        if (IsItemIn(DONELIST, str_digest))
        {
            CfOut(cf_verbose, "", " -> This promise has already been verified");
            return this;
        }

        PrependItem(&DONELIST, str_digest, NULL);
    }

/* Finally if we're supposed to ignore locks ... do the remaining stuff */

    if (IGNORELOCK)
    {
        this.lock = xstrdup("dummy");
        return this;
    }

    promise = BodyName(pp);
    snprintf(cc_operator, CF_MAXVARSIZE - 1, "%s-%s", promise, host);
    strncpy(cc_operand, operand, CF_BUFSIZE - 1);
    CanonifyNameInPlace(cc_operand);
    RemoveDates(cc_operand);

    free(promise);

    CfDebug("AcquireLock(%s,%s), ExpireAfter=%d, IfElapsed=%d\n", cc_operator, cc_operand, attr.transaction.expireafter,
            attr.transaction.ifelapsed);

    for (i = 0; cc_operator[i] != '\0'; i++)
    {
        sum = (CF_MACROALPHABET * sum + cc_operator[i]) % CF_HASHTABLESIZE;
    }

    for (i = 0; cc_operand[i] != '\0'; i++)
    {
        sum = (CF_MACROALPHABET * sum + cc_operand[i]) % CF_HASHTABLESIZE;
    }

    snprintf(cflog, CF_BUFSIZE, "%s/cf3.%.40s.runlog", CFWORKDIR, host);
    snprintf(cflock, CF_BUFSIZE, "lock.%.100s.%s.%.100s_%d_%s", pp->bundle, cc_operator, cc_operand, sum, str_digest);
    snprintf(cflast, CF_BUFSIZE, "last.%.100s.%s.%.100s_%d_%s", pp->bundle, cc_operator, cc_operand, sum, str_digest);

    CfDebug("LOCK(%s)[%s]\n", pp->bundle, cflock);

// Now see if we can get exclusivity to edit the locks

    CFINITSTARTTIME = time(NULL);

    WaitForCriticalSection();

/* Look for non-existent (old) processes */

    lastcompleted = FindLock(cflast);
    elapsedtime = (time_t) (now - lastcompleted) / 60;

    if (elapsedtime < 0)
    {
        CfOut(cf_verbose, "", " XX Another cf-agent seems to have done this since I started (elapsed=%jd)\n",
              (intmax_t) elapsedtime);
        ReleaseCriticalSection();
        return this;
    }

    if (elapsedtime < attr.transaction.ifelapsed)
    {
        CfOut(cf_verbose, "", " XX Nothing promised here [%.40s] (%jd/%u minutes elapsed)\n", cflast,
              (intmax_t) elapsedtime, attr.transaction.ifelapsed);
        ReleaseCriticalSection();
        return this;
    }

/* Look for existing (current) processes */

    if (!ignoreProcesses)
    {
        lastcompleted = FindLock(cflock);
        elapsedtime = (time_t) (now - lastcompleted) / 60;

        if (lastcompleted != 0)
        {
            if (elapsedtime >= attr.transaction.expireafter)
            {
                CfOut(cf_inform, "", "Lock %s expired (after %jd/%u minutes)\n", cflock, (intmax_t) elapsedtime,
                      attr.transaction.expireafter);

                pid_t pid = FindLockPid(cflock);

                if (KillLockHolder(cflock))
                {
                    LogLockCompletion(cflog, pid, "Lock expired, process killed", cc_operator, cc_operand);
                    unlink(cflock);
                }
                else
                {
                    CfOut(cf_error, "", "Unable to kill expired process %d from lock %s", (int)pid, cflock);
                }
            }
            else
            {
                ReleaseCriticalSection();
                CfOut(cf_verbose, "", "Couldn't obtain lock for %s (already running!)\n", cflock);
                return this;
            }
        }

        WriteLock(cflock);
    }

    ReleaseCriticalSection();

    this.lock = xstrdup(cflock);
    this.last = xstrdup(cflast);
    this.log = xstrdup(cflog);

/* Keep this as a global for signal handling */
    strcpy(CFLOCK, cflock);
    strcpy(CFLAST, cflast);
    strcpy(CFLOG, cflog);

    return this;
}

/************************************************************************/

void YieldCurrentLock(CfLock this)
{
    if (IGNORELOCK)
    {
        free(this.lock);        /* allocated in AquireLock as a special case */
        return;
    }

    if (this.lock == (char *) CF_UNDEFINED)
    {
        return;
    }

    CfDebug("Yielding lock %s\n", this.lock);

    if (RemoveLock(this.lock) == -1)
    {
        CfOut(cf_verbose, "", "Unable to remove lock %s\n", this.lock);
        free(this.last);
        free(this.lock);
        free(this.log);
        return;
    }

    if (WriteLock(this.last) == -1)
    {
        CfOut(cf_error, "creat", "Unable to create %s\n", this.last);
        free(this.last);
        free(this.lock);
        free(this.log);
        return;
    }

    LogLockCompletion(this.log, getpid(), "Lock removed normally ", this.lock, "");

    free(this.last);
    free(this.lock);
    free(this.log);
}

/************************************************************************/

bool AcquireLockByID(char *lock_id, int acquire_after_minutes)
/*
 * Much simpler than AcquireLock. Useful when you just want to check
 * if a certain amount of time has elapsed for an action since last
 * time you checked.  No need to clean up after calling this
 * (e.g. like YieldCurrentLock()).  
 *
 * WARNING: Is prone to race-conditions, both on the thread and 
 *          process level.  
 */
{
    CF_DB *dbp = OpenLock();
    
    if(dbp == NULL)
    {
        return false;
    }
    
    bool result;
    LockData lock_data;
    lock_data.process_start_time = PROCESS_START_TIME_UNKNOWN;
    
    if (ReadDB(dbp, lock_id, &lock_data, sizeof(lock_data)))
    {
        if(lock_data.time + (acquire_after_minutes * SECONDS_PER_MINUTE) < time(NULL))
        {
            result = WriteLockDataCurrent(dbp, lock_id);
        }
        else
        {
            result = false;
        }
    }
    else
    {
        result = WriteLockDataCurrent(dbp, lock_id);
    }
    
    CloseLock(dbp);

    return result;
}

/************************************************************************/

void GetLockName(char *lockname, char *locktype, char *base, Rlist *params)
{
    Rlist *rp;
    int max_sample, count = 0;

    for (rp = params; rp != NULL; rp = rp->next)
    {
        count++;
    }

    if (count)
    {
        max_sample = CF_BUFSIZE / (2 * count);
    }
    else
    {
        max_sample = 0;
    }

    strncpy(lockname, locktype, CF_BUFSIZE / 10);
    strcat(lockname, "_");
    strncat(lockname, base, CF_BUFSIZE / 10);
    strcat(lockname, "_");

    for (rp = params; rp != NULL; rp = rp->next)
    {
        strncat(lockname, (char *) rp->item, max_sample);
    }
}

/************************************************************************/

#if defined(HAVE_PTHREAD)

/************************************************************************/

static void GetMutexName(const pthread_mutex_t *mutex, char *mutexname)
{
    if (mutex >= cft_system && mutex <= cft_server_keyseen)
    {
        sprintf(mutexname, "mutex %ld", (long) (mutex - cft_system));
    }
    else
    {
        sprintf(mutexname, "unknown mutex 0x%p", mutex);
    }
}

/************************************************************************/

extern long EndMeasureValueUs(struct timespec start);

int ThreadLock(pthread_mutex_t *mutex)
{
    // start measuring on the stack
    struct timespec begin_wait = BeginMeasure();

    int result = pthread_mutex_lock(mutex);

    // save to update the global only once lock is acquired
    int offset = mutex - cft_system;
    ThreadLockMetrics *lockmetrics = THREADLOCKMETRICS + offset;

    long time_waited = EndMeasureValueUs(begin_wait);
    lockmetrics->total_waited += time_waited;
    if (time_waited > lockmetrics->max_wait)
    {
        lockmetrics->max_wait = time_waited;
    }

    if (result != 0)
    {
        char mutexname[CF_BUFSIZE];

        GetMutexName(mutex, mutexname);

        printf("!! Could not lock %s: %s\n", mutexname, strerror(result));
        return false;
    }

    lockmetrics->begin_hold = BeginMeasure();
    return true;
}

/************************************************************************/

int ThreadUnlock(pthread_mutex_t *mutex)
{
    // need to do this before actually freeing the lock, otherwise race condition
    int offset = mutex - cft_system;
    ThreadLockMetrics *lockmetrics = THREADLOCKMETRICS + offset;
    long time_held = EndMeasureValueUs(lockmetrics->begin_hold);
    lockmetrics->total_held += time_held;
    if (time_held > lockmetrics->max_held)
    {
        lockmetrics->max_held = time_held;
    }

    int result = pthread_mutex_unlock(mutex);

    if (result != 0)
    {
        char mutexname[CF_BUFSIZE];

        GetMutexName(mutex, mutexname);

        printf("!! Could not unlock %s: %s\n", mutexname, strerror(result));
        return false;
    }

    return true;
}

void DumpThreadMetrics(void)
{
    CfOut(cf_log, "", "[CFENGINE_METRICS_THREAD] >>> INSTRUMENTATION statistics BEGIN");
    CfOut(cf_log, "", "[CFENGINE_METRICS_THREAD]Lock\tTotalWait\tMaxWait\tTotalHeld\tMaxHeld");
    for (int t = 0; t < 11; ++t)
    {
        ThreadLockMetrics *lockmetrics = THREADLOCKMETRICS + t;
        CfOut(cf_log, "", "[CFENGINE_METRICS_THREAD]%d \t %ld \t %ld \t %ld \t %ld",
              t, lockmetrics->total_waited, lockmetrics->max_wait, lockmetrics->total_held, lockmetrics->max_held);
    }

    pthread_mutex_lock(&mtx_threadinstr);

    long count = threadinstr.count;
    long realtime_mean = threadinstr.realtime / count;
    long usertime_mean = threadinstr.usertime / count;
    long systime_mean = threadinstr.systime / count;
    long realtime_authed_mean = threadinstr.realtime_authed / count;
    long usertime_authed_mean = threadinstr.usertime_authed / count;
    long systime_authed_mean = threadinstr.systime_authed / count;
    long realtime_max = threadinstr.realtime_max;
    long usertime_max = threadinstr.usertime_max;
    long systime_max = threadinstr.systime_max;
    long realtime_authed_max = threadinstr.realtime_authed_max;
    long usertime_authed_max = threadinstr.usertime_authed_max;
    long systime_authed_max = threadinstr.systime_authed_max;

    /* Reset count */
    threadinstr = (struct threadinstr) { 0 };

    pthread_mutex_unlock(&mtx_threadinstr);

    CfOut(cf_log, "", "[CFENGINE_METRICS] thread times mean/max milliseconds for total of %ld threads", count);
    CfOut(cf_log, "", "[CFENGINE_METRICS] lifetime: real %ld/%ld, user %ld/%ld, system %ld/%ld",
          realtime_mean, realtime_max,
          usertime_mean, usertime_max,
          systime_mean, systime_max);
    CfOut(cf_log, "", "[CFENGINE_METRICS] authtime: real %ld/%ld, user %ld/%ld, system %ld/%ld",
          realtime_authed_mean, realtime_authed_max,
          usertime_authed_mean, usertime_authed_max,
          systime_authed_mean, systime_authed_max);

    CfOut(cf_log, "", "[CFENGINE_METRICS_THREAD]>>> INSTRUMENTATION statistics END");
}

#endif

/*****************************************************************************/
/* Level                                                                     */
/*****************************************************************************/

static time_t FindLock(char *last)
{
    time_t mtime;

    if ((mtime = FindLockTime(last)) == -1)
    {
        /* Do this to prevent deadlock loops from surviving if IfElapsed > T_sched */

        if (WriteLock(last) == -1)
        {
            CfOut(cf_error, "", "Unable to lock %s\n", last);
            return 0;
        }

        return 0;
    }
    else
    {
        return mtime;
    }
}

/************************************************************************/

int WriteLock(char *name)
{
    CF_DB *dbp;

    CfDebug("WriteLock(%s)\n", name);

    ThreadLock(cft_lock);
    if ((dbp = OpenLock()) == NULL)
    {
        ThreadUnlock(cft_lock);
        return -1;
    }

    WriteLockDataCurrent(dbp, name);

    CloseLock(dbp);
    ThreadUnlock(cft_lock);

    return 0;
}

/************************************************************************/

static bool WriteLockDataCurrent(CF_DB *dbp, char *lock_id)
{
    LockData lock_data;
    
    lock_data.pid = getpid();
    lock_data.time = time(NULL);
    lock_data.process_start_time = GetProcessStartTime(getpid());

    return WriteLockData(dbp, lock_id, &lock_data);
}

/*****************************************************************************/

static bool WriteLockData(CF_DB *dbp, char *lock_id, LockData *lock_data)
{
    if(WriteDB(dbp, lock_id, lock_data, sizeof(LockData)))
    {
        return true;
    }
    else
    {
        return false;
    }
}

/*****************************************************************************/

bool InvalidateLockTime(char *lock_id)
{
    time_t epoch = 0;
    
    CF_DB *dbp = OpenLock();

    if (dbp == NULL)
    {
        return false;
    }
    
    LockData lock_data;
    lock_data.process_start_time = PROCESS_START_TIME_UNKNOWN;

    if(!ReadDB(dbp, lock_id, &lock_data, sizeof(lock_data)))
    {
        CloseLock(dbp);
        return true;  /* nothing to invalidate */
    }
    
    lock_data.time = epoch;

    bool result = WriteLockData(dbp, lock_id, &lock_data);

    CloseLock(dbp);
    
    return result;
}

/*****************************************************************************/

static void LogLockCompletion(char *cflog, int pid, char *str, char *operator, char *operand)
{
    FILE *fp;
    char buffer[CF_MAXVARSIZE];
    struct stat statbuf;
    time_t tim;

    CfDebug("LockLogCompletion(%s)\n", str);

    if (cflog == NULL)
    {
        return;
    }

    if ((fp = fopen(cflog, "a")) == NULL)
    {
        CfOut(cf_error, "fopen", "Can't open lock-log file %s\n", cflog);
        exit(1);
    }

    if ((tim = time((time_t *) NULL)) == -1)
    {
        CfDebug("Cfengine: couldn't read system clock\n");
    }

    sprintf(buffer, "%s", cf_ctime(&tim));

    Chop(buffer);

    fprintf(fp, "%s:%s:pid=%d:%s:%s\n", buffer, str, pid, operator, operand);

    fclose(fp);

    if (cfstat(cflog, &statbuf) != -1)
    {
        if (statbuf.st_size > CFLOGSIZE)
        {
            CfOut(cf_verbose, "", "Rotating lock-runlog file\n");
            RotateFiles(cflog, 2);
        }
    }
}

/*****************************************************************************/

static int RemoveLock(char *name)
{
    CF_DB *dbp;

    if ((dbp = OpenLock()) == NULL)
    {
        return -1;
    }

    ThreadLock(cft_lock);
    DeleteDB(dbp, name);
    ThreadUnlock(cft_lock);

    CloseLock(dbp);
    return 0;
}

/************************************************************************/

time_t FindLockTime(char *name)
{
    CF_DB *dbp;
    LockData entry;
    entry.process_start_time = PROCESS_START_TIME_UNKNOWN;

    CfDebug("FindLockTime(%s)\n", name);

    if ((dbp = OpenLock()) == NULL)
    {
        return -1;
    }

    if (ReadDB(dbp, name, &entry, sizeof(entry)))
    {
        CloseLock(dbp);
        return entry.time;
    }
    else
    {
        CloseLock(dbp);
        return -1;
    }
}

/************************************************************************/

static pid_t FindLockPid(char *name)
{
    CF_DB *dbp;
    LockData entry;
    entry.process_start_time = PROCESS_START_TIME_UNKNOWN;

    if ((dbp = OpenLock()) == NULL)
    {
        return -1;
    }

    if (ReadDB(dbp, name, &entry, sizeof(entry)))
    {
        CloseLock(dbp);
        return entry.pid;
    }
    else
    {
        CloseLock(dbp);
        return -1;
    }
}

/************************************************************************/

CF_DB *OpenLock()
{
    CF_DB *dbp;

    if (!OpenDB(&dbp, dbid_locks))
    {
        return NULL;
    }

    return dbp;
}

/************************************************************************/

void CloseLock(CF_DB *dbp)
{
    if (dbp)
    {
        CloseDB(dbp);
    }
}

/*****************************************************************************/

static void RemoveDates(char *s)
{
    int i, a = 0, b = 0, c = 0, d = 0;
    char *dayp = NULL, *monthp = NULL, *sp;
    char *days[7] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
    char *months[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

// Canonifies or blanks our times/dates for locks where there would be an explosion of state

    if (strlen(s) < strlen("Fri Oct 1 15:15:23 EST 2010"))
    {
        // Probably not a full date
        return;
    }

    for (i = 0; i < 7; i++)
    {
        if ((dayp = strstr(s, days[i])))
        {
            *dayp = 'D';
            *(dayp + 1) = 'A';
            *(dayp + 2) = 'Y';
            break;
        }
    }

    for (i = 0; i < 12; i++)
    {
        if ((monthp = strstr(s, months[i])))
        {
            *monthp = 'M';
            *(monthp + 1) = 'O';
            *(monthp + 2) = 'N';
            break;
        }
    }

    if (dayp && monthp)         // looks like a full date
    {
        sscanf(monthp + 4, "%d %d:%d:%d", &a, &b, &c, &d);

        if (a * b * c * d == 0)
        {
            // Probably not a date
            return;
        }

        for (sp = monthp + 4; *sp != '\0'; sp++)
        {
            if (sp > monthp + 15)
            {
                break;
            }

            if (isdigit(*sp))
            {
                *sp = 't';
            }
        }
    }
}

/************************************************************************/

void PurgeLocks()
{
    CF_DBC *dbcp;
    char *key;
    int ksize, vsize;
    LockData entry;
    time_t now = time(NULL);

    CF_DB *dbp = OpenLock();
    
    if(!dbp)
    {
        return;
    }

    memset(&entry, 0, sizeof(entry));

    if (ReadDB(dbp, "lock_horizon", &entry, sizeof(entry)))
    {
        if (now - entry.time < SECONDS_PER_WEEK * 4)
        {
            CfOut(cf_verbose, "", " -> No lock purging scheduled");
            CloseLock(dbp);
            return;
        }
    }

    CfOut(cf_verbose, "", " -> Looking for stale locks to purge");

    if (!NewDBCursor(dbp, &dbcp))
    {
        CloseLock(dbp);
        return;
    }

    while (NextDB(dbp, dbcp, &key, &ksize, (void *) &entry, &vsize))
    {
        if (strncmp(key, "last.internal_bundle.track_license.handle",
                    strlen("last.internal_bundle.track_license.handle")) == 0)
        {
            continue;
        }

        if (now - entry.time > (time_t) CF_LOCKHORIZON)
        {
            CfOut(cf_verbose, "", " --> Purging lock (%ld) %s", now - entry.time, key);
            DBCursorDeleteEntry(dbcp);
        }
    }

    entry.time = now;
    DeleteDBCursor(dbp, dbcp);

    WriteDB(dbp, "lock_horizon", &entry, sizeof(entry));
    CloseLock(dbp);
}

/************************************************************************/
/* Release critical section                                             */
/************************************************************************/

static void WaitForCriticalSection()
{
    time_t now = time(NULL), then = FindLockTime("CF_CRITICAL_SECTION");

/* Another agent has been waiting more than a minute, it means there
   is likely crash detritus to clear up... After a minute we take our
   chances ... */

    while ((then != -1) && (now - then < 60))
    {
        sleep(1);
        now = time(NULL);
        then = FindLockTime("CF_CRITICAL_SECTION");
    }

    WriteLock("CF_CRITICAL_SECTION");
}

/************************************************************************/

static void ReleaseCriticalSection()
{
    RemoveLock("CF_CRITICAL_SECTION");
}

/************************************************************************/

int ShiftChange(void)
{
    if (IsDefinedClass("(Hr00|Hr06|Hr12|Hr18).Min00_05"))
    {
        return true;
    }
    else
    {
        return false;
    }
}
