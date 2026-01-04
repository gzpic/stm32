#include <deque>



define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "CHECK failed: %s\n", msg); \
            std::abort(); \
        } \
    } while (0)



class Scheduler {
 public:
    Scheduler() = default;
    unsigned int maxNumSeqs;
    unsigned int maxNumBatchedTokens;
    int eos;
    std::deque<int> waiting;
    std::deque<int> running;
    BlockManager* blockManager = BlockManager(config.num_kvcacheBlocks, config.kvcacheBlockSize);

    bool isFinished() const {
        return waiting.empty() && running.empty();
    }

    void addSequence(int seqId) {
        waiting.push_back(seqId);
    }

    SchedulerResult* schedule(void) {
        SchedulerResult result = {0};
        // prefill
        std::vector<int> scheduleSeqs;
        numSeqs = 0;
        numBatchedTokens = 0;
        while (!waiting.empty() && numSeqs < maxNumSeqs ) { // prefill等待数组不为空 && 当前待处理的序列数大于允许的最大序列数
           exeSeq = waiting.front();
           if (numBatchedTokens + exeSeq.size() > maxNumBatchedTokens || !blockManager.camAllocateBlock()) {
               break;
        numSeqs++;
        numBatchedTokens += exeSeq.size() - exeSeq.numCachedTokens;
        exeSeq.sts = SequenceSts::RUNNING;
        waiting.pop_front();
        running.push_back(exeSeq);

        if (!scheduleSeqs.empty()) {
            result.seqs = scheduleSeqs;
            result.isPrefill = TRUE;
            return &result;
        } 
        // decode

        while (!running.empty() && numSeqs < maxNumSeqs) { // decode运行数组不为空 && 当前待处理的序列数大于允许的最大序列数
           exeSeq = running.front();
           while (!blockManager.canAppend(exeSeq)) {
                if (!running.empty()) {
                    runningSeq = running.front();
                    preempt(runningSeq);
                    running.pop_front();
                } else {
                    preempt(exeSeq); // 自抢占，非常重要，notion有相关的笔记的
                    break;
           } else {
            numSeqs++;
            blockManager.mayAppend(exeSeq);
            scheduleSeqs.push_back(exeSeq);
        // 如果decode为空，立刻终止程序，抛出错误，防止return {}，FALSE
        CHECK(!scheduled_seqs.empty(), "Decode step produced empty batch");
        for (auto it = exeSeq.rbegin(); it != exeSeq.rend(); ++it) { // TODO decode有很多step，这一轮优先处理的下一个step仍然优先处理
            running.push_front(*it);
        } 
            running.push_front(exeSeq);
        result.seqs = scheduleSeqs;
        result.isPrefill = FALSE;
        return &result;
    }
    void preempt(Sequence& seq) {
        blockManager.deAllocate(seq);
        seq.sts = SequenceSts::DROPPED;
        // waiting.push_back(seq); // TODO 抢占是放前还是放后，非常的重要，端侧场景，被抢占的直接抛掉了
    }
   
    void postProcess(const std::vector<int> seqs, std::vector<int> tokenIds) {
        for (int i = 0; i < seqs.size() && i < tokenIds.size(); i++) {
            int seq = seqs[i];
            int tokenId = tokenIds[i];
            seq.appendToken(tokenId);
            if ((seq.ignoreEos == false && tokenId == eos)|| (seq.numCompletedTokens == seq.maxDecodeLen)) { // eos终止符
                seq.sts = SequenceSts::FINISHED;
                blockManager.deAllocate(seq); 
                auto it = std::find(running.begin(), running.end(), seq);
                if (it != running.end()){
                    running.erase(it);}
        }
    }

};