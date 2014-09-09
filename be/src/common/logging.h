// Copyright 2012 Cloudera Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.


#ifndef IMPALA_COMMON_LOGGING_H
#define IMPALA_COMMON_LOGGING_H

// This is a wrapper around the glog header.  When we are compiling to IR,
// we don't want to pull in the glog headers.  Pulling them in causes linking
// issues when we try to dynamically link the codegen'd functions.
#ifdef IR_COMPILE
#include <iostream>
  #define DCHECK(condition) while(false) std::cout
  #define DCHECK_EQ(a, b) while(false) std::cout
  #define DCHECK_NE(a, b) while(false) std::cout
  #define DCHECK_GT(a, b) while(false) std::cout
  #define DCHECK_LT(a, b) while(false) std::cout
  #define DCHECK_GE(a, b) while(false) std::cout
  #define DCHECK_LE(a, b) while(false) std::cout
  #define DCHECK_NOTNULL(a) while(false) std::cout
  // Similar to how glog defines DCHECK for release.
  #define LOG(level) while(false) std::cout
  #define VLOG(level) while(false) std::cout
  #define VLOG_IS_ON(level) (false)
#else
  // GLOG defines this based on the system but doesn't check if it's already
  // been defined.  undef it first to avoid warnings.
  // glog MUST be included before gflags.  Instead of including them,
  // our files should include this file instead.
  #undef _XOPEN_SOURCE
  #include <glog/logging.h>
  #include <gflags/gflags.h>

  #ifdef NDEBUG
  #undef DCHECK_NOTNULL
  // Fix warnings of unused variables when NDEBUG is defined (glog emits (a) for the
  // following macro which triggers the warning).
  #define DCHECK_NOTNULL(a) while(false)
  #endif
#endif

// Define verbose logging levels.  Per-row logging is ore verbase than per-file / per-rpc
// logging which is more verbose than per-connection / per-query logging.
#define VLOG_CONNECTION VLOG(1)
#define VLOG_RPC        VLOG(2)
#define VLOG_QUERY      VLOG(1)
#define VLOG_FILE       VLOG(2)
#define VLOG_ROW        VLOG(3)
#define VLOG_PROGRESS   VLOG(2)

#define VLOG_CONNECTION_IS_ON VLOG_IS_ON(1)
#define VLOG_RPC_IS_ON VLOG_IS_ON(2)
#define VLOG_QUERY_IS_ON VLOG_IS_ON(1)
#define VLOG_FILE_IS_ON VLOG_IS_ON(2)
#define VLOG_ROW_IS_ON VLOG_IS_ON(3)
#define VLOG_PROGRESS_IS_ON VLOG_IS_ON(2)

// IR modules don't use these methods, and can't see the google namespace used in
// GetFullLogFilename()'s prototype.
#ifndef IR_COMPILE
namespace impala {

// glog doesn't allow multiple invocations of InitGoogleLogging(). This method
// conditionally calls InitGoogleLogging() only if it hasn't been called before.
void InitGoogleLoggingSafe(const char* arg);

// Returns the full pathname of the symlink to the most recent log
// file corresponding to this severity
void GetFullLogFilename(google::LogSeverity severity, std::string* filename);

// Shuts down the google logging library. Call before exit to ensure that log files are
// flushed. May only be called once.
void ShutdownLogging();

// Writes all command-line flags to the log at level INFO.
void LogCommandLineFlags();
}

#endif // IR_COMPILE

#endif
