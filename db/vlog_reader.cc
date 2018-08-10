// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/vlog_reader.h"
#include <stdio.h>
#include "leveldb/env.h"
#include "util/coding.h"
#include "util/crc32c.h"
#include "util/mutexlock.h"

namespace leveldb {
namespace log {

VReader::Reporter::~Reporter() {
}

VReader::VReader(SequentialFile* file,  bool checksum,
               uint64_t initial_offset)
    : file_(file),
      reporter_ (NULL),
      checksum_(checksum),
      backing_store_(new char[kBlockSize]),
      buffer_(),
      eof_(false){
    if(initial_offset > 0)
        SkipToPos(initial_offset);
}

VReader::VReader(SequentialFile* file, Reporter* reporter, bool checksum,
               uint64_t initial_offset)
    : file_(file),
      reporter_ (reporter),
      checksum_(checksum),
      backing_store_(new char[kBlockSize]),
      buffer_(),
      eof_(false){
    if(initial_offset > 0)
        SkipToPos(initial_offset);
}

VReader::~VReader() {
    delete[] backing_store_;
    delete file_;
}

bool VReader::SkipToPos(size_t pos) {
  if (pos > 0) {
      Status skip_status = file_->SkipFromHead(pos);
    if (!skip_status.ok()) {
      ReportDrop(pos, skip_status);
      return false;
    }
  }
  return true;
}

bool VReader::ReadRecord(Slice* record, std::string* scratch, int& head_size)
{
    scratch->clear();
    record->clear();

    if(buffer_.size() < kVHeaderMaxSize)
    {
        if(!eof_)
        {
            size_t left_head_size = buffer_.size();
            if(left_head_size > 0)
                memcpy(backing_store_, buffer_.data(), left_head_size);
            buffer_.clear();
            Status status = file_->Read(kBlockSize - left_head_size, &buffer_, backing_store_ + left_head_size);

            if(left_head_size > 0)
                buffer_.go_back(left_head_size);

            if (!status.ok())
            {
                buffer_.clear();
                ReportDrop(kBlockSize, status);
                eof_ = true;
                return false;
            }
            else if(buffer_.size() < kBlockSize)
            {
                eof_ = true;
                if(buffer_.size() < 4 + 1 + 1)
                   return false;
            }
        }
        else
        {
            if(buffer_.size() < 4 + 1 + 1)
            {
                buffer_.clear();
                return false;
            }
        }
    }
    uint64_t length = 0;
    uint32_t expected_crc = crc32c::Unmask(DecodeFixed32(buffer_.data()));
    buffer_.remove_prefix(4);
    const char *varint64_begin = buffer_.data();
    bool b = GetVarint64(&buffer_, &length);
    assert(b);
    head_size = 4 + (buffer_.data() - varint64_begin);
    if(length <= buffer_.size())
    {
        if (checksum_) {
            uint32_t actual_crc = crc32c::Value(buffer_.data(), length);
            if (actual_crc != expected_crc) {
                ReportCorruption(head_size + length, "checksum mismatch");
                return false;
            }
        }
        *record = Slice(buffer_.data(), length);
        buffer_.remove_prefix(length);
        return true;
    }
    else
    {
        if(eof_ == true)
        {
            return false;
        }
        scratch->reserve(length);
        size_t buffer_size = buffer_.size();
        scratch->assign(buffer_.data(), buffer_size);
        buffer_.clear();
        const uint64_t left_length = length - buffer_size;
        if(left_length > kBlockSize/2)
        {
            Slice buffer;
            scratch->resize(length);
            Status status = file_->Read(left_length, &buffer, const_cast<char*>(scratch->data()) + buffer_size);

            if(!status.ok())
            {
                ReportDrop(left_length, status);
                return false;
            }
            if(buffer.size() < left_length)
            {
                eof_ = true;
                scratch->clear();

                return false;
            }
        }
        else
        {
            Status status = file_->Read(kBlockSize, &buffer_, backing_store_);

            if(!status.ok())
            {
                ReportDrop(kBlockSize, status);
                return false;
            }
            else if(buffer_.size() < kBlockSize)
            {
                if(buffer_.size() < left_length)
                {
                    eof_ = true;
                    scratch->clear();
                    ReportCorruption(left_length, "last record not full");
                    return false;////////////////////////////////
                }
                eof_ = true; 
            }
            scratch->append(buffer_.data(), left_length);
            buffer_.remove_prefix(left_length);
        }
        if (checksum_) {
            uint32_t actual_crc = crc32c::Value(scratch->data(), length);
            if (actual_crc != expected_crc) {
               ReportCorruption(head_size + length, "checksum mismatch");
                return false;
            }
        }
        *record = Slice(*scratch);
        return true;
    }
}

bool VReader::Read(char* val, size_t size, size_t pos)
{
    Slice buffer;
   // Status status = file_->Read(size, &buffer, val);
    Status status =  file_->Pread(pos, size, &buffer, val);
    if (!status.ok() || buffer.size() != size)
    {
        ReportDrop(size, status);
        return false;
    }
    return true;
}

void VReader::ReportCorruption(uint64_t bytes, const char* reason) {
  ReportDrop(bytes, Status::Corruption(reason));
}

void VReader::ReportDrop(uint64_t bytes, const Status& reason) {
  if (reporter_ != NULL)
  {
    reporter_->Corruption(static_cast<size_t>(bytes), reason);
  }
}

bool VReader::DeallocateDiskSpace(uint64_t offset, size_t len)
{
    return file_->DeallocateDiskSpace(offset, len).ok();
}

}  // namespace log
}  // namespace leveldb
