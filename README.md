**WiscKey启发于FAST会议发表了论文 WiscKey: Separating Keys from Valuesin SSD-conscious Storage.是基于LSM（Log-Structured Merge-Tree）结构并且采用kv分离存储思路实现的存储引擎，WiscKey是基于leveddb 1.20版本进行修改的，WiscKey并不是按照论文里的设计来实现的，只是借鉴了kv分离的思想**

# 特点
 * 相比leveldb，WiscKey的写入速度更快，因为kv分离后减少了写放大，合并的效率大大提高，特别是对大kv场景这种优势更加明显.
  
 * 相比leveldb，WiscKey的读速度可能稍慢一些，因为kv分离带来的坏处就是需要多一次从磁盘读取value的操作，但因为kv分离使sst文件能容纳的kv记录更多了，大大减少了sst文件个数，便于tablecache以及blockcache容纳更多的kv记录，因此读速度并没有想像中那么差.
  * 相比leveldb，WiscKey不支持快照机制
  * leveldb虽然官方文档上写Keys and values are arbitrary byte arrays.但其实细看代码发现Keys and values的长度均需小于4G（详见PutLengthPrefixedSlice函数）,WiscKey与leveldb一样，Keys and values的长度均需小于4G

# 写流程
与leveldb不同的是，写入memtable以及sst文件的记录其实是<key,address>,address指向这条kv操作的日志记录在vlog文件中的偏移以及长度。
![image](https://github.com/joker-qi/WiscKey/raw/master/images/put.png)
插入或者删除：先append到vLog末尾，再将Key和日志记录的地址插入LSM

# 读流程
比leveldb多了一步，因为从memtable或者sst文件读到的v其实是address，需要从address所指的位置读出真正的value。

简而言之，就是LSM中获得地址，vLog中读取

# 如何保证一致性
leveldb的实现里，当immemtbale刷入到sst文件并且成功在manifest文件中记录下这次变化后，log文件就可以被删除掉了。否则，在恢复的时候，数据库会重新回放该log文件。

由前面可知WiscKey的vlog文件时不能删除的，因为各个value的内容还存放在这里，那么故障恢复时该怎么做，不可能从头到尾回放vlog，WiscKey的做法是在imm生成sst文件前，在imm中加入一条kv记录<"head", pos>,pos的含义就是在vlog中的偏移，代表在pos以前的kv记录已经成功写入到了sst文件，恢复的时候先Get("head")获得pos，然后从pos位置处回放vlog文件即可。

# 被用户删除或者过期版本的Value的空间回收
Compaction过程需要被删除的数据由于只是删除了Key，Value还保留在分开的vlog文件中，这就需要异步的回收。LSM本身的Compaction其实也是垃圾回收。

先看一下WiscKey论文的回收思路
- 离线回收：扫描整个LSM对Value Log进行mark and sweep，但这相当于给系统带来了负载带来了陡峭的波峰
- 在线回收：其中head的位置是新的Block插入的位置，tail是Value回收操作的开始位置，垃圾回收过程被触发后，顺序从Tail开始读取Block，将有效的Block插入到Head。删除空间并后移Tail。
![image](https://github.com/joker-qi/WiscKey/raw/master/images/garbage.png)

这种在线回收的缺点：
- 什么时候触发？根据删除修改请求的多少？如何知道删除修改请求了多少次？（如果数据库关闭了重新打开呢，之前的删除修改次数怎么获取）
- 什么时候终止？WiscKey论文里的设计是只有一个vlog文件，利用fallocate系统调用实现tail之前磁盘空间的归还，但并没有提什么时候终止？
- 不够灵活，如果垃圾（过期版本的kv日志记录）主要集中在vlog文件的某一块位置呢，这样从头开始对vlog文件进行垃圾回收显然效率太低


本项目WiscKey中设计的垃圾回收机制
- 什么时候触发？根据lsm Compaction（指的是major Compaction）的情况，因为每次Compaction后就可以知道lsm已经清除了多少条过期版本的kv记录，只不过这些过期版本的kv日志记录还在vlog文件中，当lsm已经清除的过期版本的kv记录数达到我们自己设定的某个阀值时，便触发在线回收。为了防止崩溃或者db关闭，需要定期将lsm已经清除的过期版本的kv记录数持久化，这里采用put("vloginfo",lsm已经清理的垃圾总数)。
- 依然没有解决不够灵活的问题，为了解决该问题采取多vlog文件，一个vlog文件到达一定大小后，生成下一个vlog文件。
- 因为采用了多vlog文件，这就自然而然解决了如何终止垃圾回收的问题，把整个vlog文件扫描完后便可终止。
- 什么样的vlog文件该进行垃圾回收呢？举个例子，假如一个vlog文件能够容纳1000条kv日志记录，其中500条kv日志记录已经在lsm中清除过了，意味着该vlog文件中至少有一半的日志记录已经过期失效。我们可以在option中设置vlog文件可以容忍的过期失效记录总数的上限。超过该上限便进行该vlog文件的垃圾回收。

# vlog垃圾回收的过程

从头到尾扫描vlog文件的每一条kv日志记录（一次读很大一块vlog文件的内容）
- 如果是条put记录，则GetPtr(key)获取addr，如果addr所指的位置就是当前日志记录的位置（省去读具体value值，也就减少了一次read磁盘），说明该kv日志记录有效，重新将该kv记录put到db中，否则直接丢弃该记录（可以维护一个有效的writebatch,用来存放vlog文件回放出来的多条有效kv记录，当writebatch够大时在一口气write到db中）。vlog文件扫描完后便可删除该vlog文件。
- 如果是条delete记录，直接丢弃。

# 引入多vlog文件后WiscKey的改变
![image](https://github.com/joker-qi/WiscKey/raw/master/images/put1.png)
- address里多出来了file_numb用来表示value所在的vlog文件的编号
- 在imm生成sst文件前，在imm中加入一条kv记录<"head", <file_numb,pos>>,pos的含义就是在vlog中的偏移，代表在pos以前的kv记录已经成功写入到了sst文件,file_numb代表的是从哪个vlog文件进行恢复
- 为了防止崩溃或者db关闭，需要定期将lsm已经清除的过期版本的kv记录数持久化，用put("vloginfo",各个vlog文件中已经被lsm清理的垃圾记录总数)。

# 性能
 
 ## 测试环境1
 创建一个100万条kv记录的数据库，其中每条记录的key为16个字节，value为100个字节，我们没有开启snappy压缩，所以写入磁盘的文件大小就是数据的原始大小，其余参数都是option的默认值。
 
    LevelDB:    version 1.20
    Date:       Mon Jan 29 17:39:13 2018
    CPU:        24 * Intel(R) Xeon(R) CPU E5-2620 0 @ 2.00GHz
    CPUCache:   15360 KB
    Keys:       16 bytes each
    Values:     100 bytes each (50 bytes after compression)
    Entries:    1000000
    Raw Size:   110.6 MB (estimated)
    File Size:  62.9 MB (estimated)
    WARNING: Snappy compression is not enabled

 ## 写性能

 leveldb
 
    fillseq      :       5.817 micros/op;   19.0 MB/s     
    fillsync     :   16151.499 micros/op;    0.0 MB/s (1000 ops)
    fillrandom   :      11.370 micros/op;    9.7 MB/s     
    overwrite    :      15.211 micros/op;    7.3 MB/s
 
 WiscKey
 
    fillseq      :       6.461 micros/op;   17.1 MB/s     
    fillsync     :   16034.868 micros/op;    0.0 MB/s (1000 ops)
    fillrandom   :       7.848 micros/op;   14.1 MB/s     
    overwrite    :       8.467 micros/op;   13.1 MB/s

 分析：可以看出fillseq（顺序写，即各sst文件直接没有范围冲突）WiscKey并不占优势，因为是顺序写，所以对应leveldb，各个sst文件间没有范围冲突，因此可以快速合并。但对于WiscKey，因为在dump immemtable生成level0文件时，会插入一条head记录，这就导致了第0层的文件总是范围冲突的，需要合并，所以WiscKey慢，以后的版本会在这里优化。 fillrandom（随机写，即各sst文件范围冲突概率很大）以及overwrite（覆盖写，即各sst文件范围冲突概率很大），因为WiscKey是kv分离的，所以合并速度快于leveldb，自然fillrandom以及overwrite更快。
 
