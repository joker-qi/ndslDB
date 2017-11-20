#include "db/garbage_collector.h"
#include "leveldb/slice.h"
#include "db/write_batch_internal.h"
#include "db/db_impl.h"
#include "fcntl.h"
#include "db/filename.h"

namespace leveldb{

void GarbageCollector::SetVlog(uint64_t vlog_number, uint64_t garbage_beg_pos)
{
    SequentialFile* vlr_file;
    db_->options_.env->NewSequentialFile(VLogFileName(db_->dbname_, vlog_number), &vlr_file);
    vlog_reader_ = new log::VReader(vlr_file, true,0);
    vlog_number_ = vlog_number;
    garbage_pos_ = garbage_beg_pos;
}

void GarbageCollector::BeginGarbageCollect()
{
    uint64_t garbage_pos = garbage_pos_;
    Slice record;
    std::string str;
    WriteOptions write_options;
    if(garbage_pos_ > 0)
    {
        if(!vlog_reader_->SkipToPos(garbage_pos_))//从指定位置开始回收
        {
            Log(db_->options_.info_log,"clean vlog %lu false because of SkipToPos %lu false",vlog_number_,garbage_pos_);
        }
    }
    Log(db_->options_.info_log,"begin clean %lu in %lu\n", garbage_pos_, vlog_number_);

    Slice key,value;
    WriteBatch batch, clean_valid_batch;
    std::string val;
    bool isEndOfFile = false;
    while(!db_->IsShutDown())//db关了
    {
        int head_size = 0;
        if(!vlog_reader_->ReadRecord(&record, &str, head_size))//读日志记录读取失败了
        {
            isEndOfFile = true;
            break;
        }

        garbage_pos_ += head_size;
        WriteBatchInternal::SetContents(&batch, record);//会把record的内容拷贝到batch中去
        ReadOptions read_options;
        uint64_t size = record.size();//size是整个batch的长度，包括batch头
        uint64_t pos = 0;//是相对batch起始位置的偏移
        uint64_t old_garbage_pos = garbage_pos_;
        while(pos < size)//遍历batch看哪些kv有效
        {
            bool isDel = false;
            Status s =WriteBatchInternal::ParseRecord(&batch, pos, key, value, isDel);//解析完一条kv后pos是下一条kv的pos
            assert(s.ok());
            garbage_pos_ = old_garbage_pos + pos;

            //log文件里的delete记录可以直接丢掉，因为sst文件会记录
            if(!isDel && db_->GetPtr(read_options, key, &val).ok())
            {
                uint64_t code = DecodeFixed64(val.data());
                size_t size = code & 0xffffff;
                code = code>>24;
                uint64_t file_numb = code & 0xff;
                uint64_t item_pos = code>>8;
                if(item_pos + size == garbage_pos_ && file_numb == vlog_number_ )
                {
                    clean_valid_batch.Put(key, value);
                }
            }
        }
        assert(pos == size);
        if(WriteBatchInternal::ByteSize(&clean_valid_batch) > db_->options_.clean_write_buffer_size)
        {//clean_write_buffer_size必须要大于12才行，12是batch的头部长，创建batch或者clear batch后的初始大小就是12
            Status s = db_->Write(write_options, &clean_valid_batch);
            assert(s.ok());
            clean_valid_batch.Clear();
        }
    }

#ifndef NDEBUG
    Log(db_->options_.info_log,"tail is %lu, last key is %s, ;last value is %s\n", garbage_pos_,key.data(),value.data());
    if(db_->IsShutDown())
        Log(db_->options_.info_log," clean stop by shutdown\n");
    else if(isEndOfFile)
        Log(db_->options_.info_log," clean stop by read end\n");
    else
        Log(db_->options_.info_log," clean stop by unknown reason\n");
#endif
    if(WriteBatchInternal::Count(&clean_valid_batch) > 0)
    {
        Status s = db_->Write(write_options, &clean_valid_batch);
        assert(s.ok());
        clean_valid_batch.Clear();
    }

    if(garbage_pos_ - garbage_pos > 0)
    {
        if(isEndOfFile)
        {
            std::string file_name = VLogFileName(db_->dbname_, vlog_number_);
            db_->env_->DeleteFile(file_name);
            Log(db_->options_.info_log,"clean vlog %lu ok and delete it\n", vlog_number_);
        }
        else
        {
            vlog_reader_->DeallocateDiskSpace(garbage_pos, garbage_pos_ - garbage_pos);
            char buf[8];
            Slice v(buf, 8);
            EncodeFixed64(buf, (garbage_pos_ << 24) | vlog_number_ );
            Log(db_->options_.info_log,"clean vlog %lu stop in %lu \n", vlog_number_, garbage_pos_);
            Status s = db_->Put(write_options, "tail", v);//head不会出现在vlog中，但tail会
            assert(s.ok());
     //这里有个坑，put不一定成功如果是因为数据库正在关闭而退出上述循环，这时候插入tail会失败
     //因为makeroom会返回失败，因为合并操作会将bg_error_设置为io error,为了填坑，我把因为数据库关闭而引起的bg_error
     //设为特殊的error，详见db_impl.cc的MakeRoomForWrite函数
        }
    }
}
}
