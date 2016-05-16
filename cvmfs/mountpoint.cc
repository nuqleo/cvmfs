/**
 * This file is part of the CernVM File System.
 */

#include "cvmfs_config.h"
#include "mountpoint.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>

#include "backoff.h"
#include "duplex_sqlite3.h"
#include "file_chunk.h"
#include "globals.h"
#include "glue_buffer.h"
#include "logging.h"
#include "lru.h"
#include "options.h"
#include "platform.h"
#include "sqlitemem.h"
#include "statistics.h"
#include "util_concurrency.h"
#include "util/pointer.h"
#include "util/posix.h"
#include "util/string.h"
#include "uuid.h"

using namespace std;  // NOLINT


const char *FileSystem::kDefaultCacheBase = "/var/lib/cvmfs";


bool FileSystem::CheckCacheMode() {
  if ((cache_mode_ & kCacheAlien) && (cache_mode_ & kCacheShared)) {
    boot_error_ = "Failure: shared local disk cache and alien cache mutually "
                  "exclusive. Turn off shared local disk cache.";
    boot_status_ = loader::kFailOptions;
    return false;
  }
  if ((cache_mode_ & kCacheAlien) && (cache_mode_ & kCacheShared)) {
    boot_error_ = "Failure: quota management and alien cache mutually "
                  "exclusive. Turn off quota limit.";
    boot_status_ = loader::kFailOptions;
    return false;
  }
  if ((cache_mode_ & kCacheAlien) && (cache_mode_ & kCacheNfs)) {
    boot_error_ = "Failure: NFS cache mode and alien cache mutually exclusive.";
    boot_status_ = loader::kFailOptions;
    return false;
  }

  if (type_ == kFsLibrary) {
    if (cache_mode_ & (kCacheShared | kCacheNfs | kCacheManaged)) {
      boot_error_ = "Failure: libcvmfs supports only unmanaged exclusive cache "
                    "or alien cache.";
      boot_status_ = loader::kFailOptions;
      return false;
    }
  }

  return true;
}


FileSystem *FileSystem::Create(
  const string &name,
  FileSystem::Type type,
  OptionsManager *options_mgr)
{
  UniquePtr<FileSystem>
    file_system(new FileSystem(name, type, options_mgr));

  file_system->SetupLogging();
  LogCvmfs(kLogCvmfs, kLogDebug, "Options:\n%s", options_mgr->Dump().c_str());
  file_system->SetupSqlite();

  file_system->DetermineCacheMode();
  if (!file_system->CheckCacheMode())
    return file_system.Release();
  file_system->DetermineCacheDirs();
  file_system->DetermineWorkspace();

  file_system->boot_status_ = loader::kFailOk;
  return file_system.Release();
}


void FileSystem::DetermineCacheDirs() {
  string optarg;

  cache_dir_ = kDefaultCacheBase;
  if (options_mgr_->GetValue("CVMFS_CACHE_BASE", &optarg))
    cache_dir_ = MakeCanonicalPath(optarg);

  if (cache_mode_ & kCacheShared) {
    cache_dir_ += "/shared";
  } else {
    cache_dir_ += "/" + name_;
  }

  if (options_mgr_->GetValue("CVMFS_ALIEN_CACHE", &optarg))
    alien_cache_dir_ = optarg;
}


void FileSystem::DetermineCacheMode() {
  string optarg;

  if (options_mgr_->GetValue("CVMFS_SHARED_CACHE", &optarg) &&
      options_mgr_->IsOn(optarg))
  {
    cache_mode_ = kCacheShared;
  } else {
    cache_mode_ = kCacheExclusive;
  }
  if (options_mgr_->GetValue("CVMFS_ALIEN_CACHE", &optarg)) {
    cache_mode_ |= kCacheAlien;
  }
  if (options_mgr_->GetValue("CVMFS_NFS_SOURCE", &optarg)) {
    cache_mode_ |= kCacheNfs;
    if (options_mgr_->GetValue("CVMFS_NFS_SHARED", &optarg)) {
      cache_mode_ |= kCacheNfsHa;
    }
  }
  if (options_mgr_->GetValue("CVMFS_SERVER_CACHE_MODE", &optarg) &&
      options_mgr_->IsOn(optarg))
  {
    cache_mode_ |= kCacheNoRename;
  }

  if (options_mgr_->GetValue("CVMFS_QUOTA_LIMIT", &optarg))
    quota_limit_ = String2Int64(optarg) * 1024 * 1024;
  if (quota_limit_ > 0)
    cache_mode_ |= kCacheManaged;
}


void FileSystem::DetermineWorkspace() {
  workspace_ = cache_dir_;
}


FileSystem::FileSystem(
  const string &name,
  FileSystem::Type type,
  OptionsManager *options_mgr)
  : name_(name)
  , type_(type)
  , options_mgr_(options_mgr)
  , fd_workspace_lock_(-1)
  , found_crash_(false)
  , cache_mode_(0)
  , quota_limit_(kDefaultQuotaLimit)
{
  g_uid = geteuid();
  g_gid = getegid();

  string optarg;
  if (options_mgr_->GetValue("CVMFS_SERVER_CACHE_MODE", &optarg) &&
      options_mgr_->IsOn(optarg))
  {
    g_raw_symlinks = true;
  }
}


FileSystem::~FileSystem() {
  if (!path_crash_guard_.empty())
    unlink(path_crash_guard_.c_str());
  if (!path_workspace_lock_.empty())
    unlink(path_workspace_lock_.c_str());
  if (fd_workspace_lock_ >= 0)
    UnlockFile(fd_workspace_lock_);
  sqlite3_shutdown();
  SqliteMemoryManager::CleanupInstance();
  SetLogSyslogPrefix("");
  SetLogMicroSyslog("");
  SetLogDebugFile("");
}


bool FileSystem::LockWorkspace() {
  path_workspace_lock_ = workspace_ + "/lock." + name_;
  fd_workspace_lock_ = TryLockFile(path_workspace_lock_);
  if (fd_workspace_lock_ == -1) {
    boot_error_ = "could not acquire workspace lock (" +
                 StringifyInt(errno) + ")";
    boot_status_ = loader::kFailCacheDir;
    return false;
  } else if (fd_workspace_lock_ == -2) {
    // Prevent double mount
    // Hack at this point (TODO(jblomer))
//    string fqrn;
//    retval = platform_getxattr(*cvmfs::mountpoint_, "user.fqrn", &fqrn);
//    if (!retval) {
      fd_workspace_lock_ = LockFile(path_workspace_lock_);
      if (fd_workspace_lock_ < 0) {
        boot_error_ = "could not acquire workspace lock (" +
                     StringifyInt(errno) + ")";
        boot_status_ = loader::kFailCacheDir;
        return false;
      }
/*    } else {
      if (fqrn == *cvmfs::repository_name_) {
        LogCvmfs(kLogCvmfs, kLogDebug | kLogSyslogWarn,
                 "repository already mounted on %s",
                 cvmfs::mountpoint_->c_str());
        return loader::kFailDoubleMount;
      } else {
        LogCvmfs(kLogCvmfs, kLogDebug | kLogSyslogErr,
                 "CernVM-FS repository %s already mounted on %s",
                 fqrn.c_str(), cvmfs::mountpoint_->c_str());
        return loader::kFailOtherMount;
      }
    }*/
  }
  return true;
}


void FileSystem::LogSqliteError(
  void *user_data __attribute__((unused)),
  int sqlite_extended_error,
  const char *message)
{
  int log_dest = kLogDebug;
  int sqlite_error = sqlite_extended_error & 0xFF;
  switch (sqlite_error) {
    case SQLITE_INTERNAL:
    case SQLITE_PERM:
    case SQLITE_NOMEM:
    case SQLITE_IOERR:
    case SQLITE_CORRUPT:
    case SQLITE_FULL:
    case SQLITE_CANTOPEN:
    case SQLITE_MISUSE:
    case SQLITE_FORMAT:
    case SQLITE_NOTADB:
      log_dest |= kLogSyslogErr;
      break;
    case SQLITE_WARNING:
    case SQLITE_NOTICE:
    default:
      break;
  }
  LogCvmfs(kLogCvmfs, log_dest, "SQlite3: %s (%d)",
           message, sqlite_extended_error);
}


bool FileSystem::SetupCrashGuard() {
  path_crash_guard_ = workspace_ + "/running." + name_;
  platform_stat64 info;
  int retval = platform_stat(path_crash_guard_.c_str(), &info);
  if (retval == 0) {
    found_crash_ = true;
    string msg = "looks like cvmfs has been crashed previously";
    if (cache_mode_ & kCacheManaged) {
      msg += ", rebuilding cache database";
    }
    LogCvmfs(kLogCvmfs, kLogDebug | kLogSyslogWarn, "%s", msg.c_str());
  }
  retval = open(path_crash_guard_.c_str(), O_RDONLY | O_CREAT, 0600);
  if (retval < 0) {
    boot_error_ = "could not open running sentinel (" +
                  StringifyInt(errno) + ")";
    boot_status_ = loader::kFailCacheDir;
    return false;
  }
  close(retval);
  return true;
}


void FileSystem::SetupLogging() {
  string optarg;
  if (options_mgr_->GetValue("CVMFS_SYSLOG_LEVEL", &optarg))
    SetLogSyslogLevel(String2Uint64(optarg));
  if (options_mgr_->GetValue("CVMFS_SYSLOG_FACILITY", &optarg))
    SetLogSyslogFacility(String2Int64(optarg));
  if (options_mgr_->GetValue("CVMFS_USYSLOG", &optarg))
    SetLogMicroSyslog(optarg);
  if (options_mgr_->GetValue("CVMFS_DEBUGLOG", &optarg))
    SetLogDebugFile(optarg);
  SetLogSyslogPrefix(name_);
}


void FileSystem::SetupSqlite() {
  // Make sure SQlite starts clean after initialization
  sqlite3_shutdown();

  int retval;
  retval = sqlite3_config(SQLITE_CONFIG_LOG, FileSystem::LogSqliteError, NULL);
  assert(retval == SQLITE_OK);
  retval = sqlite3_config(SQLITE_CONFIG_MULTITHREAD);
  assert(retval == SQLITE_OK);
  SqliteMemoryManager::GetInstance()->AssignGlobalArenas();

  // Disable SQlite3 file locking
  retval = sqlite3_vfs_register(sqlite3_vfs_find("unix-none"), 1);
  assert(retval == SQLITE_OK);
}


bool FileSystem::SetupWorkspace() {
  const int mode = (workspace_ == alien_cache_dir_) ? 0770 : 0700;
  if (!MkdirDeep(workspace_, mode, false)) {
    boot_error_ = "cannot create cache directory " + cache_dir_;
    boot_status_ = loader::kFailCacheDir;
    return false;
  }

  if (!LockWorkspace())
    return false;
  if (!SetupCrashGuard())
    return false;

  return true;
}


//------------------------------------------------------------------------------



MountPoint *MountPoint::Create(
  const string &fqrn,
  FileSystem *file_system)
{
  string optarg;
  UniquePtr<MountPoint> mountpoint(new MountPoint(fqrn, file_system));

  // At this point, we have a repository name, the type (fuse or library),
  // an options manager from the FileSystem object and a newly generated uuid
  // for the mount point

  mountpoint->CreateStatistics();
  mountpoint->CreateTables();

  mountpoint->SetupTtls();
  mountpoint->backoff_throttle_ = new BackoffThrottle();

  mountpoint->boot_status_ = loader::kFailOk;
  return mountpoint.Release();
}


void MountPoint::CreateStatistics() {
  statistics_ = new perf::Statistics();

  // Register the ShortString's static counters
  statistics_->Register("pathstring.n_instances", "Number of instances");
  statistics_->Register("pathstring.n_overflows", "Number of overflows");
  statistics_->Register("namestring.n_instances", "Number of instances");
  statistics_->Register("namestring.n_overflows", "Number of overflows");
  statistics_->Register("linkstring.n_instances", "Number of instances");
  statistics_->Register("linkstring.n_overflows", "Number of overflows");

  if (file_system_->type() == FileSystem::kFsFuse) {
    statistics_->Register("inode_tracker.n_insert",
                          "overall number of accessed inodes");
    statistics_->Register("inode_tracker.n_remove",
                          "overall number of evicted inodes");
    statistics_->Register("inode_tracker.no_reference",
                          "currently active inodes");
    statistics_->Register("inode_tracker.n_hit_inode",
                          "overall number of inode lookups");
    statistics_->Register("inode_tracker.n_hit_path",
                          "overall number of successful path lookups");
    statistics_->Register("inode_tracker.n_miss_path",
                          "overall number of unsuccessful path lookups");
  }

  // Callback counters
  n_fs_open_ = statistics_->Register("cvmfs.n_fs_open",
                                     "Overall number of file open operations");
  n_fs_dir_open_ = statistics_->Register("cvmfs.n_fs_dir_open",
                   "Overall number of directory open operations");
  n_fs_lookup_ = statistics_->Register("cvmfs.n_fs_lookup",
                                       "Number of lookups");
  n_fs_lookup_negative_ = statistics_->Register("cvmfs.n_fs_lookup_negative",
                                                "Number of negative lookups");
  n_fs_stat_ = statistics_->Register("cvmfs.n_fs_stat", "Number of stats");
  n_fs_read_ = statistics_->Register("cvmfs.n_fs_read", "Number of files read");
  n_fs_readlink_ = statistics_->Register("cvmfs.n_fs_readlink",
                                         "Number of links read");
  n_fs_forget_ = statistics_->Register("cvmfs.n_fs_forget",
                                       "Number of inode forgets");
  n_io_error_ = statistics_->Register("cvmfs.n_io_error",
                                      "Number of I/O errors");
  no_open_files_ = statistics_->Register("cvmfs.no_open_files",
                                         "Number of currently opened files");
  no_open_dirs_ = statistics_->Register("cvmfs.no_open_dirs",
                  "Number of currently opened directories");
}


void MountPoint::CreateTables() {
  if (file_system_->type() != FileSystem::kFsFuse) {
    md5path_cache_ = new lru::Md5PathCache(kLibPathCacheSize, statistics_);
    return;
  }

  directory_handles_ = new FuseDirectoryHandles();
  directory_handles_->set_empty_key((uint64_t)(-1));
  directory_handles_->set_deleted_key((uint64_t)(-2));
  chunk_tables_ = new ChunkTables();

  string optarg;
  OptionsManager *options_manager = file_system_->options_mgr();
  uint64_t mem_cache_size = kDefaultMemcacheSize;
  if (options_manager->GetValue("CVMFS_MEMCACHE_SIZE", &optarg))
    mem_cache_size = String2Uint64(optarg) * 1024 * 1024;

  const double memcache_unit_size =
    (static_cast<double>(kInodeCacheFactor) * lru::Md5PathCache::GetEntrySize())
    + lru::InodeCache::GetEntrySize() + lru::PathCache::GetEntrySize();
  const unsigned memcache_num_units =
    mem_cache_size / static_cast<unsigned>(memcache_unit_size);
  // Number of cache entries must be a multiple of 64
  const unsigned mask_64 = ~((1 << 6) - 1);
  inode_cache_ = new lru::InodeCache(memcache_num_units & mask_64, statistics_);
  path_cache_ = new lru::PathCache(memcache_num_units & mask_64, statistics_);
  md5path_cache_ = new lru::Md5PathCache((memcache_num_units * 7) & mask_64,
                                         statistics_);

  inode_tracker_ = new glue::InodeTracker();
}


unsigned MountPoint::GetMaxTtlMn() {
  MutexLockGuard lock_guard(lock_max_ttl_);
  return max_ttl_sec_ / 60;
}


MountPoint::MountPoint(const string &fqrn, FileSystem *file_system)
  : fqrn_(fqrn)
  , file_system_(file_system)
  , uuid_mountpoint_(cvmfs::Uuid::Create(""))
  , n_fs_open_(NULL)
  , n_fs_dir_open_(NULL)
  , n_fs_lookup_(NULL)
  , n_fs_lookup_negative_(NULL)
  , n_fs_stat_(NULL)
  , n_fs_read_(NULL)
  , n_fs_readlink_(NULL)
  , n_fs_forget_(NULL)
  , n_io_error_(NULL)
  , no_open_files_(NULL)
  , no_open_dirs_(NULL)
  , statistics_(NULL)
  , directory_handles_(NULL)
  , chunk_tables_(NULL)
  , inode_cache_(NULL)
  , path_cache_(NULL)
  , md5path_cache_(NULL)
  , inode_tracker_(NULL)
  , backoff_throttle_(NULL)
  , max_ttl_sec_(kDefaultMaxTtlSec)
  , kcache_timeout_sec_(static_cast<double>(kDefaultKCacheTtlSec))
{
  int retval = pthread_mutex_init(&lock_max_ttl_, NULL);
  assert(retval == 0);
}


MountPoint::~MountPoint() {
  pthread_mutex_destroy(&lock_max_ttl_);

  delete backoff_throttle_;
  delete inode_tracker_;
  delete md5path_cache_;
  delete path_cache_;
  delete inode_cache_;
  delete chunk_tables_;
  delete directory_handles_;
  sqlite3_shutdown();
  SqliteMemoryManager::CleanupInstance();
  delete statistics_;
  delete uuid_mountpoint_;
}


void MountPoint::SetMaxTtlMn(unsigned value_minutes) {
  MutexLockGuard lock_guard(lock_max_ttl_);
  max_ttl_sec_ = value_minutes * 60;
}


void MountPoint::SetupTtls() {
  OptionsManager *options_manager = file_system_->options_mgr();
  string optarg;

  if (options_manager->GetValue("CVMFS_MAX_TTL", &optarg))
    SetMaxTtlMn(String2Uint64(optarg));

  if (options_manager->GetValue("CVMFS_KCACHE_TIMEOUT", &optarg)) {
    // Can be negative and should then be interpreted as 0.0
    kcache_timeout_sec_ =
      std::max(0.0, static_cast<double>(String2Int64(optarg)));
  }
  LogCvmfs(kLogCvmfs, kLogDebug, "kernel caches expire after %d seconds",
           static_cast<int>(kcache_timeout_sec_));
}
