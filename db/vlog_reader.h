// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_DB_VLOG_READER_H_
#define STORAGE_LEVELDB_DB_VLOG_READER_H_

#include <stdint.h>

#include "db/log_format.h"
#include "leveldb/slice.h"
#include "leveldb/status.h"
#include "port/port.h"
namespace leveldb {

class SequentialFile;

namespace log {
class VReader {
 public:
  class Reporter {
   public:
    virtual ~Reporter();

    // Some corruption was detected.  "size" is the approximate number
    // of bytes dropped due to the corruption.
    virtual void Corruption(size_t bytes, const Status& status) = 0;
  };

  VReader(SequentialFile* file, bool checksum, uint64_t initial_offset=0);
  VReader(SequentialFile* file, Reporter* reporter, bool checksum, uint64_t initial_offset=0);

  ~VReader();

  bool Read(char* val, size_t size, size_t pos);
  bool ReadRecord(Slice* record, std::string* scratch, int& head_size);
  bool SkipToPos(size_t pos);
  bool DeallocateDiskSpace(uint64_t offset, size_t len);

 private:
////  port::Muex mutex_;
  SequentialFile* const file_;      // which file to read
  Reporter* const reporter_;        // error reporter
  bool const checksum_;             // should we do data checksum
  char* const backing_store_;       // read buffer
  Slice buffer_;
  bool eof_;   // Last Read() indicated EOF by returning < kBlockSize
  // Reports dropped bytes to the reporter.
  // buffer_ must be updated to remove the dropped bytes prior to invocation.
  void ReportCorruption(uint64_t bytes, const char* reason);
  void ReportDrop(uint64_t bytes, const Status& reason);
  // No copying allowed
  VReader(const VReader&);
  void operator=(const VReader&);
};

}  // namespace log
}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_LOG_READER_H_
