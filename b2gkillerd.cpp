#include <cutils/properties.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <sys/un.h>
#include <sys/socket.h>

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <functional>
#include <memory>
#include <vector>
#include <set>

#ifdef ANDROID
#include "android/log.h"
#define LOGD(...)                                                \
  if (debugging_b2g_killer) {                                            \
    __android_log_print(ANDROID_LOG_INFO, "b2gkillerd", ## __VA_ARGS__); \
  }
#define LOGI(...) __android_log_buf_print(LOG_ID_SYSTEM, ANDROID_LOG_INFO, "b2gkillerd", ## __VA_ARGS__);
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "b2gkillerd", ## __VA_ARGS__);
#else
#define LOGD(...) printf(## __VA_ARGS__)
#define LOGI(...) printf(## __VA_ARGS__)
#define LOGE(...) fprintf(stderr, ## __VA_ARGS__)
#endif

/**
 * Classes:
 *
 *    MemPressureWatcher
 *          |
 *          | <<callback>>
 *          v
 *        killer -----> MemPressureCounter
 *          |
 *          |
 *          +---> ProcessKiller -----> ProcessList
 *          |
 *          |
 *          +---> KickGCCC()
 *
 * MemPressureWatcher:
 *
 *   It listens events of memory.pressure_level of the root cgroup and
 *   pass the counter to the callback.  The counter is increased
 *   whenever memory pressure is low/medium/or high.  So far, it
 *   listens medium pressure.
 *
 * MemPressureCounter:
 *
 *   It implements a counter for memory pressure.  The counter is an
 *   exponential moving average of the number of events over time.
 *
 * ProcessList:
 *
 *   It keeps a list of process and reads memory usages of processes
 *   from the procfs.
 *
 * ProcessKiller:
 *
 *   It pick one of background process to kill, according types and
 *   memory usages.
 *
 * KickGCCC:
 *
 *   Send a signal to the parent process to start GC/CC.
 */


#define ASSERT(x, msg) do { \
    if (!(x)) { \
      LOGE("ASSERT! ERROR: %s : %s:%d\n", \
           (msg), __FILE__, __LINE__); abort(); \
    } \
  } while (0)

#ifdef ANDROID
#define CGROUP_FS "/dev/memcg/"
#else
#define CGROUP_FS "/sys/fs/cgroup/memory/"
#endif

#define B2G_CGROUP CGROUP_FS "b2g/"
#define FG_CGROUP B2G_CGROUP "fg/"
#define BG_CGROUP B2G_CGROUP "bg/"
#define TRY_TO_KEEP_CGROUP BG_CGROUP "try_to_keep/"
#define DEFAULT_CGROUP B2G_CGROUP "default/"

#define MEMORY_PRESSURE_LEVEL_PATH CGROUP_FS "memory.pressure_level"
#define EVENT_CONTROL CGROUP_FS "cgroup.event_control"


// Data points will degrade by EMA_FACTOR every second.
// Half life period = ln(0.5) / ln(EMA_FACTOR_DFL)
// new_value = old_value * EMA_FACTOR_DFL ^ n-seconds
#define EMA_FACTOR_DFL 0.1
#define PRESSURE_LEVEL "medium"
#define MEMINFO_PATH "/proc/meminfo"
#define MEMINFO_FIELD_COUNT 9
#define PID_FILE "/data/local/tmp/b2gkillerd.pid"
#define LOWEST_SCORE 1.0

static double mem_pressure_high_threshold = 60.0;
static double mem_pressure_low_threshold = 40.0;

// Trigger GC/CC only when the memory pressure is between max and min
// threshold below.
static double gc_cc_max = 40.0;
static double gc_cc_min = 30.0;

/**
  * Dirty memory have to be written out before reclaiming it while
  * clean memory can be reclaimed and used without additional cost.
  * So, we have to weigh dirty memory differently.
  */
static double dirty_mem_weight = 0.2;

enum KilleeType {
  HIGH_SWAP_BACKGROUND,
  HIGH_SWAP_TRY_TO_KEEP,
  HIGH_SWAP_FOREGROUND,
  BACKGROUND,
  TRY_TO_KEEP
};

/**
  * For the case of zram, swapped pages take memory too.  They are
  * compressed, so it shoud multiply a weight.
  */
static double swapped_mem_weight = 0.5;


// High swapped mem weight.
static double high_swapped_mem_weight = 1.5;

// Consecutive kicks should be longer than |kMinKickInterval| seconds.
static double min_kick_interval = 0.5;

// Available swap free threshold.
static double swap_free_soft_threshold = 0.25;
static double swap_free_hard_threshold = 0.20;

static bool debugging_b2g_killer = false;
static bool enable_dumpping_process_info = false;

union meminfo {
  struct {
    int64_t nr_free_pages;
    int64_t cached;
    int64_t swap_cached;
    int64_t buffers;
    int64_t shmem;
    int64_t unevictable;
    int64_t total_swap;
    int64_t free_swap;
    int64_t dirty;
  } field;
  int64_t arr[MEMINFO_FIELD_COUNT];
};

static const char* field_names[MEMINFO_FIELD_COUNT] = {
  "MemFree:",
  "Cached:",
  "SwapCached:",
  "Buffers:",
  "Shmem:",
  "Unevictable:",
  "SwapTotal:",
  "SwapFree:",
  "Dirty:",
};

enum field_match_result {
  NO_MATCH,
  PARSE_FAIL,
  PARSE_SUCCESS
};

static bool ParseInt64(const char* str, int64_t* ret) {
  char* endptr;
  long long val = strtoll(str, &endptr, 10);
  if (str == endptr) {
    return false;
  }
  *ret = (int64_t)val;
  return true;
}

static enum field_match_result MatchField(const char* cp, const char* ap,
                                          const char* const field_names[],
                                          int field_count, int64_t* field,
                                          int *field_idx) {
  int i;
  for (i = 0; i < field_count; i++) {
    if (!strcmp(cp, field_names[i])) {
      *field_idx = i;
      return ParseInt64(ap, field) ? PARSE_SUCCESS : PARSE_FAIL;
    }
  }
  return NO_MATCH;
}

static bool MemInfoParseLine(char *line, union meminfo *mi) {
  char *cp = line;
  char *ap;
  char *save_ptr;
  int64_t val;
  int field_idx;
  enum field_match_result match_res;
  cp = strtok_r(line, " ", &save_ptr);
  if (!cp) {
    return false;
  }
  ap = strtok_r(NULL, " ", &save_ptr);
  if (!ap) {
    return false;
  }
  match_res = MatchField(cp, ap, field_names, MEMINFO_FIELD_COUNT,
                         &val, &field_idx);
  if (match_res == PARSE_SUCCESS) {
    mi->arr[field_idx] = val;
  }
  return (match_res != PARSE_FAIL);
}

static int MemInfoParse(union meminfo *mi) {
  size_t sz = 128;
  char *buf = (char*)malloc(sz);
  memset(mi, 0, sizeof(union meminfo));

  FILE* fo = fopen(MEMINFO_PATH, "r");
  if (fo == nullptr) {
    return -1;
  }

  while (getline(&buf, &sz, fo) >= 0) {
    if (!MemInfoParseLine(buf, mi)) {
      LOGI("meminfo parse error");
      return -1;
    }
  }

  free(buf);
  fclose(fo);

  return 0;
}

/**
 * Counting memory pressure with a moving average.
 */
class MemPressureCounter {
public:
  MemPressureCounter(double aMul = EMA_FACTOR_DFL) : factor(aMul), datum(0.0) {
  }
  MemPressureCounter(const MemPressureCounter& aOther)
    : factor(aOther.factor),
      datum(aOther.datum),
      last_tm(aOther.last_tm) {
  }

  double HalfLifePeriod() {
    return log(0.5) / log(factor);
  }

  double Average() { return datum; }

  double AddOne() {
    datum = exponentialMA(datum, 1.0, DiffLastTimestamp(), factor);
    return datum;
  }

  double Add(double addend) {
    datum = exponentialMA(datum, addend, DiffLastTimestamp(), factor);
    return datum;
  }

  double ForceUpdate() {
    datum = exponentialMA(datum, 0, DiffLastTimestamp(), factor);
    return datum;
  }

private:
  double factor;
  double datum;
  timespec last_tm;

  double DiffLastTimestamp() {
    timespec tm;
    clock_gettime(CLOCK_REALTIME_COARSE, &tm);
    double diff_sec =
      (double)(tm.tv_sec - last_tm.tv_sec) +
      (double)(tm.tv_nsec - last_tm.tv_nsec) * 1.0e-9;
    last_tm = tm;

    return diff_sec;
  }

  /**
   * Exponential Moving Average.
   *
   * https://en.wikipedia.org/wiki/Moving_average#Exponential_moving_average
   */
  static double
  exponentialMA(double old, double addend, double elapse, double factor) {
    double new_value = addend;
    if (old > 0.0) {
      new_value += old * pow(factor, elapse);
    }
    return new_value;
  }
};


/**
 * The information (memory usages) of a process.
 */
class ProcessInfo {
public:
  ProcessInfo(int aPid) : mValid(true), mPid(aPid) {}

  bool IsValid() const { return mValid; }

  int GetPid() const { return mPid; }

  /**
   * Read memory usages of the process from procfs.
   */
  bool Update() {
    bool success = UpdateStatus();
    if (!success) {
      return false;
    }
    success = UpdateComm();
    if (!success) {
      return false;
    }
    return UpdateSmaps();
  }

private:
  bool UpdateComm() {
    // Read app name
    char commpath[64];
    snprintf(commpath, 64, "/proc/%d/comm", mPid);
    std::unique_ptr<FILE, int(*)(FILE*)> commfp(fopen(commpath, "r"), fclose);
    if (commfp == nullptr) {
      mValid = false;
      return false;
    }

    char _appname[64];
    const char *appname;
    int cp = fread(_appname, 1, 63, commfp.get());
    if (cp > 0) {
      // Remove trailing spaces
      while (cp > 0 && isspace(_appname[cp - 1])) {
        cp--;
      }
      _appname[cp] = 0;
      appname = _appname;
    } else {
      appname = "<unknown>";
    }

    strncpy(mAppName, appname, 64);
    return true;
  }

  bool UpdateStatus() {
    char status_fn[64];
    snprintf(status_fn, 64, "/proc/%d/status", mPid);
    std::unique_ptr<FILE, int(*)(FILE*)> fo(fopen(status_fn, "r"), fclose);
    if (fo == nullptr) {
      mValid = false;
      return false;
    }

    size_t sz = 128;
    char *buf = (char*)malloc(sz);
    while (getline(&buf, &sz, fo.get()) >= 0) {
      char *saveptr;
      char *field = strtok_r(buf, " \t\n:", &saveptr);
      if (strcmp(field, "VmSize") == 0) {
        char *value = strtok_r(nullptr, " \t\n:", &saveptr);
        mVmSize = atol(value);
      } else if (strcmp(field, "VmRSS") == 0) {
        char *value = strtok_r(nullptr, " \t\n:", &saveptr);
        mVmRSS = atol(value);
      } else if (strcmp(field, "RssAnon") == 0) {
        char *value = strtok_r(nullptr, " \t\n:", &saveptr);
        mRssAnon = atol(value);
      } else if (strcmp(field, "RssFile") == 0) {
        char *value = strtok_r(nullptr, " \t\n:", &saveptr);
        mRssFile = atol(value);
      } else if (strcmp(field, "RssShmem") == 0) {
        char *value = strtok_r(nullptr, " \t\n:", &saveptr);
        mRssShmem = atol(value);
      } else if (strcmp(field, "VmSwap") == 0) {
        char *value = strtok_r(nullptr, " \t\n:", &saveptr);
        mVmSwap = atol(value);
      } else if (strcmp(field, "State") == 0) {
        char *value = strtok_r(nullptr, " \t\n:", &saveptr);
        strncpy(&mState, value, 1);
      }
    }
    free(buf);
    return true;
  }

  bool UpdateSmaps() {
    char status_fn[64];
    snprintf(status_fn, 64, "/proc/%d/smaps", mPid);
    std::unique_ptr<FILE, int(*)(FILE*)> fo(fopen(status_fn, "r"), fclose);
    if (fo == nullptr) {
      mValid = false;
      return false;
    }

    mSharedClean = 0;
    mSharedDirty = 0;
    mPrivateClean = 0;
    mPrivateDirty = 0;

    size_t sz = 128;
    char *buf = (char*)malloc(sz);
    while (getline(&buf, &sz, fo.get()) >= 0) {
      char *saveptr;
      char *field = strtok_r(buf, " \t\n:", &saveptr);
      if (strcmp(field, "Shared_Clean") == 0) {
        char *value = strtok_r(nullptr, " \t\n:", &saveptr);
        mSharedClean += atol(value);
      } else if (strcmp(field, "Shared_Dirty") == 0) {
        char *value = strtok_r(nullptr, " \t\n:", &saveptr);
        mSharedDirty += atol(value);
      } else if (strcmp(field, "Private_Clean") == 0) {
        char *value = strtok_r(nullptr, " \t\n:", &saveptr);
        mPrivateClean += atol(value);
      } else if (strcmp(field, "Private_Dirty") == 0) {
        char *value = strtok_r(nullptr, " \t\n:", &saveptr);
        mPrivateDirty += atol(value);
      }
    }
    free(buf);
    return true;
  }

  bool mValid;
  int mPid;

public:
  // from status
  long mVmSize;
  long mVmRSS;
  long mRssAnon;
  long mRssFile;
  long mRssShmem;
  long mVmSwap;

  // from smaps
  long mSharedClean;
  long mSharedDirty;
  long mPrivateClean;
  long mPrivateDirty;

  char mAppName[64];
  char mState;
};


/**
 * Keep a list of processes of foreground, background and try_to_keep.
 */
class ProcessList {
public:
  typedef std::vector<ProcessInfo> ListType;

  ProcessList() {}

  bool HasProc(int aPid) {
    return find_proc_itr(aPid) != mProcs.end();
  }

  // Make this process a foreground processess
  void AddFG(int aPid) {
    add_proc(aPid);
    mFgProc.insert(aPid);
  }
  int HasFG(int aPid) const {
    return mFgProc.find(aPid) != mFgProc.end();
  }

  // Make this process a foreground processess
  void AddBG(int aPid) {
    add_proc(aPid);
    mBgProc.insert(aPid);
  }
  int HasBG(int aPid) const {
    return mBgProc.find(aPid) != mBgProc.end();
  }

  // Try to keep this bacground processes
  void AddTryToKeep(int aPid) {
    add_proc(aPid);
    mTryToKeepSet.insert(aPid);
  }

  void UpdateInfo() {
    for (auto& pinfo : mProcs) {
      pinfo.Update();
    }
  }

  void RemoveProc(int aPid) {
    for (auto itr = mProcs.begin(); itr != mProcs.end(); ++itr) {
      if (itr->GetPid() == aPid) {
        mProcs.erase(itr);
        mTryToKeepSet.erase(aPid);
        mFgProc.erase(aPid);
        mBgProc.erase(aPid);
        break;
      }
    }
  }

  const ProcessInfo* GetProcInfo(int aPid) const {
    auto itr = find_proc_itr(aPid);
    if (itr != mProcs.end()) {
      return &*itr;
    }
    return nullptr;
  }

  ListType::const_iterator begin() const {
    return mProcs.begin();
  }

  ListType::const_iterator end() const {
    return mProcs.end();
  }

  bool HasTryToKeep(int aPid) const {
    return mTryToKeepSet.find(aPid) != mTryToKeepSet.end();
  }

  size_t size() const {
    return mProcs.size();
  }

private:
  typedef std::set<int> ProcIdSet;

  ListType mProcs;
  ProcIdSet mTryToKeepSet;
  ProcIdSet mFgProc;
  ProcIdSet mBgProc;

  std::vector<ProcessInfo>::const_iterator find_proc_itr(int aPid) const {
    // Assuming this list is short, it doesn't spend much time.
    for (auto itr = mProcs.begin(); itr != mProcs.end(); itr++) {
      if (itr->GetPid() == aPid) {
        return itr;
      }
    }
    return mProcs.end();
  }

  void add_proc(int aPid) {
    ASSERT(!HasProc(aPid), "add an existing process");
    mProcs.push_back(ProcessInfo(aPid));
  }
};


/**
 * Pick a process to kill.
 *
 * This class actually has no any states.  It is just a collection of
 * all functions that implement this feature.
 */
class ProcessKiller {
  /**
   * Add proccess processes to a ProcessList.  The are found from
   * cgroups of b2g/fg, b2g/bg, and b2g/bg/try_to_keep.  They are
   * belong to foreground, background, and try_to_keep processes.
   *
   * When the memory falls short, it try to kill a process picked from
   * background process to release memory.  There are try_to_keep
   * processes.  They are background processes too, but we try to keep
   * them in the memory if possible, so other background processes will
   * be killed before the try_to_keep processes.
   */
  static void FillB2GProcessList(ProcessList* aProcs) {
    size_t lsz = 128;
    char *line = (char*)malloc(lsz);

    FILE* fp = fopen(BG_CGROUP "cgroup.procs", "r");
    ASSERT(fp != nullptr, "Can't open background cgroup");
    while (getline(&line, &lsz, fp) > 0) {
      auto pid = atoi(line);
      aProcs->AddBG(pid);
    }
    fclose(fp);

    fp = fopen(TRY_TO_KEEP_CGROUP "cgroup.procs", "r");
    ASSERT(fp != nullptr, "Can't open try_to_keep cgroup");
    while (getline(&line, &lsz, fp) > 0) {
      auto pid = atoi(line);
      if (aProcs->HasProc(pid)) {
        // Since b2gkillerd and procmanager are asynchronous,
        // procmanager can change cgroup.procs inbetween readings
        // here.  Remove previous added info to do our best.
        aProcs->RemoveProc(pid);
      }
      aProcs->AddTryToKeep(pid);
    }
    fclose(fp);

    fp = fopen(FG_CGROUP "cgroup.procs", "r");
    ASSERT(fp != nullptr, "Can't open foreground cgroup");
    while (getline(&line, &lsz, fp) > 0) {
      auto pid = atoi(line);
      if (aProcs->HasProc(pid)) {
        // Since b2gkillerd and procmanager are asynchronous,
        // procmanager can change cgroup.procs inbetween readings
        // here.  Remove previous added info to do our best.
        //
        // FG is more important to previous groups, so adding to the
        // FG group is prefered.
        aProcs->RemoveProc(pid);
      }
      aProcs->AddFG(pid);
    }
    fclose(fp);

    free(line);

    aProcs->UpdateInfo();
  }

  static double ProcInfoKillScore(const ProcessInfo& aPInfo, bool aSwapSensitive) {
    // Prefer the processes that has more clean private pages over
    // processes that has dirty pages sicne dirty ones should be
    // written out before using it.
    //
    // Shared memory is not counted here for that it may not make more
    // available memory from shared memory to kill a process.
    double score  = aPInfo.mPrivateClean +
                    aPInfo.mPrivateDirty * dirty_mem_weight;

    // Prefer the higher SWAP space consumer to be killed.
    if (aSwapSensitive) {
      score  += aPInfo.mVmSwap * high_swapped_mem_weight;
    } else {
      score  += aPInfo.mVmSwap * swapped_mem_weight;
    }

    return score;
  }

  static bool
  IsTargetKillee(const ProcessInfo& aProc, ProcessList *aProcs, KilleeType aType) {
    if (!aProc.IsValid()) {
      // Return false if process' information is invalid.
      return false;
    } else if (aProc.mState != 'S' && aProc.mState != 'R') {
      // If the process's state is 'D' (Uninterruptible Sleep), it's high
      // possibilty that we cannot kill process and reclaim the memory
      // immediately. If the state is 'Z' (Zombie), it means process is in the end
      // of its life cycle, we should not kill it again.
      // So we skip it if state of process is not 'S' (Sleep) nor 'R' (Running).
      return false;
    } else if ((aProcs->HasTryToKeep(aProc.GetPid()) && (aType == TRY_TO_KEEP)) ||
        (aProcs->HasTryToKeep(aProc.GetPid()) && (aType == HIGH_SWAP_TRY_TO_KEEP)) ||
        (aProcs->HasBG(aProc.GetPid()) && (aType == BACKGROUND)) ||
        (aProcs->HasBG(aProc.GetPid()) && (aType == HIGH_SWAP_BACKGROUND)) ||
        (aProcs->HasFG(aProc.GetPid()) && (aType == HIGH_SWAP_FOREGROUND))) {
      return true;
    } else {
      return false;
    }
  }

  static const ProcessInfo*
  FindBestProcToKill(ProcessList* aProcs, KilleeType aType) {
    const ProcessInfo *best = nullptr;
    double best_score = 0.0;
    bool swapSensetive = (aType == HIGH_SWAP_BACKGROUND) ||
                         (aType == HIGH_SWAP_FOREGROUND) ||
                         (aType == HIGH_SWAP_TRY_TO_KEEP);
    LOGI("Try to kill process with priority %d\n", aType);
    for (auto& proc : *aProcs) {
      if (!IsTargetKillee(proc, aProcs, aType)) {
        continue;
      }

      // Set launcher app as lowest score to let it stay longer.
      double score = strcmp(proc.mAppName, "launcher") ?
                     ProcInfoKillScore(proc, swapSensetive) : LOWEST_SCORE;

      if (score > best_score) {
        best = &proc;
        best_score = score;
      }
    }

    return best;
  }

public:
  static const char* GetProcessPriority(ProcessList* aProcs, int aPid) {
    if (aProcs->HasFG(aPid)) {
      return "fg";
    } else if (aProcs->HasBG(aPid)) {
      return "bg";
    } else if (aProcs->HasTryToKeep(aPid)) {
      return "try_to_keep";
    } else {
      return "default";
    }
  }

  static void DumpProcessesInfo(ProcessList* aProcs) {
    int pid;
    const char* priority;
    for (auto proc = aProcs->begin(); proc != aProcs->end(); proc++) {
      pid = proc->GetPid();
      priority = GetProcessPriority(aProcs, pid);
      LOGI("name: %s, pid: %d, state: %c, priority: %s, score: %.2lf/%.2lf, "
           "RSS: %ld, VSS: %ld\n",
           proc->mAppName, pid, proc->mState, priority,
           ProcInfoKillScore(*proc, false), ProcInfoKillScore(*proc, true),
           proc->mVmRSS, proc->mVmSize);
    }
  }
  /**
   * Choice one of processes and kill it. Return true if kill successfully.
   *
   * Basing on the information from cgroup filesystem, it pick one of
   * background process to kill.  It will kill one of background
   * processes other than try-to-keep ones, unless only try-to-keeps
   * ones are left.
   */
  static bool KillOneProc(KilleeType aType) {
    ProcessList procs;
    FillB2GProcessList(&procs);
    auto proc = FindBestProcToKill(&procs, aType);
    if (proc == nullptr) {
      LOGD("There is no proper process to kill!\n");
      return false;
    }
    int pid = proc->GetPid();
    kill(pid, SIGKILL);

    if (enable_dumpping_process_info) {
      DumpProcessesInfo(&procs);
    }
    // XXX: Don't touch line before talking with the data team.
    //      They want the format of this log to be fixed.
    LOGI("Kill proc %s (%d) for memory pressure:%d:%ld/%ld\n",
         proc->mAppName, pid, (int)aType, proc->mVmRSS, proc->mVmSize);
    return true;
  }
};



/**
 * Watch at memory pressure events.
 *
 * It will call the callback whenever the memory pressure is above a
 * level; high, medium or low.  The callback should be assigned by
 * calling |init()|.
 *
 * EXAMPLE:
 *   MemPressureWatcher watcher;
 *   watcher.init(callback);
 *   watcher.watch(); // |callback| will be called for memory pressure.
 */
class MemPressureWatcher {
public:
  using Handler = std::function<bool(unsigned int)>;

  MemPressureWatcher()
    : mEventFd(-1),
      mMemPressureLevelFd(-1) {
  }

  ~MemPressureWatcher() {
    if (mEventFd >= 0) {
      close(mEventFd);
    }
    if (mMemPressureLevelFd >= 0) {
      close(mMemPressureLevelFd);
    }
  }

  void Init(Handler&& aHandler) {
    int eflags = 0;
    int efd = eventfd(0, eflags);

    int mpl_fd = open(MEMORY_PRESSURE_LEVEL_PATH, O_RDWR);

    char msg[256];
    snprintf(msg, 256, "%d %d " PRESSURE_LEVEL "\n", efd, mpl_fd);
    int cfd = open(EVENT_CONTROL, O_RDWR);
    int cp = 0;
    cp = write(cfd, msg, strlen(msg));
    ASSERT(cp >= 0, "Can not write to the event control");
    close(cfd);

    mEventFd = efd;
    mMemPressureLevelFd = mpl_fd;

    mHandler = std::move(aHandler);
  }

  void Watch() {
    pollfd fds[1] = {{mEventFd, POLLIN, 0}};
    int wait = 1000; // 1s
    while (poll(fds, 1, wait) >= 0) {
      uint64_t cnt = 0;

      if (fds[0].revents) {
        int cp = read(mEventFd, &cnt, 8);
        ASSERT(cp > 0, "Fail to read from the event fd");
      }
      fds[0].revents = 0;

      bool cont = mHandler(cnt);
      if (!cont) {
        break;
      }
    }
  }

private:
  int mEventFd;
  int mMemPressureLevelFd;
  Handler mHandler;
};


#define KICK_GCCC_PATH "/dev/socket/kickgccc"

class GCCCKicker {
  static int FindB2GParent() {
    int parent = -1;

    size_t commsz = 128;
    char *comm = (char*)malloc(commsz);

    size_t lsz = 128;
    char *line = (char*)malloc(lsz);
    FILE *fp = fopen(DEFAULT_CGROUP "cgroup.procs", "r");
    ASSERT(fp != nullptr, "Can't open parent cgroup");
    while (getline(&line, &lsz, fp) > 0) {
      int pid = atoi(line);
      if (pid <= 0) continue;

      snprintf(comm, commsz, "/proc/%d/comm", pid);
      std::unique_ptr<FILE, int(*)(FILE*)> commfp(fopen(comm, "r"), fclose);
      if (!commfp) continue;
      if (getline(&comm, &commsz, commfp.get()) <= 0) continue;
      if (strcmp(comm, "b2g\n")) continue;
      // The B2G parent process
      parent = pid;
      break;
    }
    fclose(fp);
    free(line);
    free(comm);

    return parent;
  }

  /**
   * Send a message to the "/dev/socket/kickgccc" socket.
   */
  static void DoKickGCCC(int aParent) {
    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, KICK_GCCC_PATH, strlen(KICK_GCCC_PATH) + 1);

    for (int i = 0; i < 2; i++) {
      do {
        if (mSO == -1) {
          mSO = socket(AF_UNIX, SOCK_DGRAM, 0);
          if (mSO < 0) {
            perror("socket");
            return;
          }
          if (connect(mSO, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            perror("connect");
            fprintf(stderr, "Fail to connect to " KICK_GCCC_PATH "\n");
            break;
          }
        }

        if (send(mSO, &aParent, sizeof(aParent), 0) != sizeof(aParent)) {
          perror("send");
          fprintf(stderr, "Fail to send to " KICK_GCCC_PATH "\n");
          break;
        }

        return;
      } while(0);

      close(mSO);
      mSO = -1;
    }
  }

public:
  // Request the parent process to kick GC/CC.
  static void Kick() {
    // Consecutive kicks should be longer than |min_kick_interval| seconds.
    timespec tm;
    clock_gettime(CLOCK_REALTIME_COARSE, &tm);
    double tm_diff = (double)(tm.tv_sec - mLastTm.tv_sec) +
      (double)(tm.tv_nsec - mLastTm.tv_nsec) * 1e-9;
    if (tm_diff < min_kick_interval) {
      return;
    }

    auto parent = FindB2GParent();
    if (parent != -1) {
      DoKickGCCC(parent);
      LOGI("b2gkillerd has kicked %d\n", parent);
      mLastTm = tm;
    } else {
      LOGD("Can not find the parent process of B2G.\n");
    }
  }

private:
  static int mSO;
  static timespec mLastTm;
};

int GCCCKicker::mSO = -1;
timespec GCCCKicker::mLastTm = { 0, 0};


void WatchMemPressure() {
  std::unique_ptr<MemPressureCounter> mpcounter =
    std::make_unique<MemPressureCounter>();

  LOGD("Start watching memory pressure events!\n");
  LOGD("The half life of the memory pressure counter is %fs.\n",
               mpcounter->HalfLifePeriod());
  LOGD("Kill processes once the counter is over %f.\n\n",
               mem_pressure_low_threshold);

  MemPressureWatcher watcher;

  /* Use moving semantic is better to pass mpcounter to the closure,
   * but the moving constructor of closures seem not able to handle it
   * correctly.
   */
  auto killer = [&](unsigned int cnt) -> bool {
    union meminfo mi;

    if (MemInfoParse(&mi) < 0) {
      LOGI("Failed to get free memory to parse meminfo!");
    }

    float swap_free_percent = (float) mi.field.free_swap / mi.field.total_swap;

    if (swap_free_percent < swap_free_soft_threshold) {
      // Swap too much, now purge time. Kill until we got more SWAP space.
      LOGI("SWAP free percentage is low, memory swap free percentage: %f\n", swap_free_percent);
      LOGD("mi.field.free_swap: %" PRIi64, mi.field.free_swap);
      LOGD("mi.field.total_swap: %" PRIi64, mi.field.total_swap);

      if (!ProcessKiller::KillOneProc(HIGH_SWAP_BACKGROUND)) {
        if (!ProcessKiller::KillOneProc(HIGH_SWAP_TRY_TO_KEEP) &&
            (swap_free_percent < swap_free_hard_threshold)) {
          ProcessKiller::KillOneProc(HIGH_SWAP_FOREGROUND);
        }
      }
    } else {
      mpcounter->Add(cnt);
      double mem_pressure_avg = mpcounter->Average();
      bool memory_too_low = mem_pressure_avg > mem_pressure_low_threshold;

      if (memory_too_low && (ProcessKiller::KillOneProc(BACKGROUND))) {
        LOGD("memory pressure counter %u, average %f, "
             "killing background app successully\n", cnt, mem_pressure_avg);
      } else if ((mem_pressure_avg > mem_pressure_high_threshold) &&
                 (ProcessKiller::KillOneProc(TRY_TO_KEEP))) {
        LOGD("memory pressure counter %u, average %f, "
             "killing try_to_keep app successfully\n", cnt, mem_pressure_avg);
      } else {
        LOGD("memory pressure counter %u, average %f\n", cnt, mem_pressure_avg);
      }

      bool do_gc_cc = (mem_pressure_avg >= gc_cc_min &&
                       mem_pressure_avg <= gc_cc_max);
      if (do_gc_cc) {
        GCCCKicker::Kick();
      }

      ASSERT(!do_gc_cc || !memory_too_low,
             "should not do both GC/CC and killing a process");
    }

    return true;
  };
  watcher.Init(MemPressureWatcher::Handler(std::move(killer)));

  watcher.Watch();
}


/**
 * Check if the directories of cgroup are setted properly.
 */
bool CheckCgroups() {
  struct stat st;
  if (stat(B2G_CGROUP, &st) < 0) {
    fprintf(stderr, B2G_CGROUP " does not exist!\n");
    return false;
  }
  if (stat(FG_CGROUP, &st) < 0) {
    fprintf(stderr, FG_CGROUP " does not exist!\n");
    return true;
  }
  if (stat(BG_CGROUP, &st) < 0) {
    fprintf(stderr, BG_CGROUP " does not exist!\n");
    return true;
  }
  if (stat(TRY_TO_KEEP_CGROUP, &st) < 0) {
    fprintf(stderr, TRY_TO_KEEP_CGROUP " does not exist!\n");
    return true;
  }
  if (stat(DEFAULT_CGROUP, &st) < 0) {
    fprintf(stderr, DEFAULT_CGROUP " does not exist!\n");
    return true;
  }
  return false;
}


int
main() {
  #ifdef ANDROID
  debugging_b2g_killer = property_get_bool("ro.b2gkillerd.debug", false);

  enable_dumpping_process_info =
    property_get_bool("ro.b2gkillerd.dump_process_info", false);

  char buf[PROPERTY_VALUE_MAX] = {'\0'};
  property_get("ro.b2gkillerd.mem_pressure_low_threshold", buf, "40.0");
  mem_pressure_low_threshold = atof(buf);

  property_get("ro.b2gkillerd.mem_pressure_high_threshold", buf, "60.0");
  mem_pressure_high_threshold = atof(buf);

  property_get("ro.b2gkillerd.gc_cc_max", buf, "40.0");
  gc_cc_max = atof(buf);

  property_get("ro.b2gkillerd.gc_cc_min", buf, "30.0");
  gc_cc_min = atof(buf);

  property_get("ro.b2gkillerd.dirty_mem_weight", buf, "0.2");
  dirty_mem_weight = atof(buf);

  property_get("ro.b2gkillerd.swapped_mem_weight", buf, "0.5");
  swapped_mem_weight= atof(buf);

  property_get("ro.b2gkillerd.min_kick_interval", buf, "0.5");
  min_kick_interval = atof(buf);

  property_get("ro.b2gkillerd.swap_free_soft_threshold", buf, "0.25");
  swap_free_soft_threshold = atof(buf);

  property_get("ro.b2gkillerd.swap_free_hard_threshold", buf, "0.20");
  swap_free_hard_threshold = atof(buf);

  LOGD("Reading config: mem_pressure_low_threshold: %f, "
       "mem_pressure_high_threshold: %fgc_cc_max: %f, gc_cc_min: %f, "
       "dirty_mem_weight: %f, swapped_mem_weight: %f, minkickinterval: %f, "
       "swap_free_soft_threshold: %f, swap_free_hard_threshold: %f\n",
       mem_pressure_low_threshold, mem_pressure_high_threshold, gc_cc_max,
       gc_cc_min, dirty_mem_weight, swapped_mem_weight, min_kick_interval,
       swap_free_soft_threshold, swap_free_hard_threshold);
 #endif

  if (CheckCgroups()) {
    return 255;
  }

  // Create/or update the PID file.
  auto pid_fp = fopen(PID_FILE, "w+");
  ASSERT(pid_fp, "Fail to create PID file " PID_FILE "\n");
  int cp = fprintf(pid_fp, "%d\n", getpid());
  ASSERT(cp > 0, "Fail to write to the PID file " PID_FILE "\n");
  fclose(pid_fp);

  WatchMemPressure();

  return 0;
}
