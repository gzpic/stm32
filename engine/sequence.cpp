



enum class SequenceSts {
    WAITING,
    RUNNING,
    FINISHED
};

class Sequence {
public:

    auto blockSize = 256
    std::vector<int> tokenIds;

    Sequence(std::vector<int32_t> tokenId, SamplingParams samplePara);//todo

    















    seqId = // TODO
    status = SequenceSts::WAITING;
    tokenId = tokenId;
    lastToken = tokenId[-1];
    numTokens = tokenId.size();
    numPromptTokens = tokenId.size(); // TODO
    numCachedTokens = 0;
    blockTable;
    temperature = samplePara.temperature;
    maxTokens = samplePara.maxTokens;
    ignoreEos = samplePara.ignoreEos;

    size_t size() {return tokenIds.size()};
    int32_t operator[](size_t i) const {return tokenIds[i]}; // 只读版本 ，const,这个函数只读对象，不允许改任何成员状态



    int32_t& operator[](size_t i) { // 可写版本
    return token_ids[i];

    



        // ===== @property: is_finished =====
    bool is_finished() const {
        return status == SequenceStatus::FINISHED;
    }

    // ===== @property: num_completion_tokens =====
    size_t num_completion_tokens() const {
        return num_tokens - num_prompt_tokens;
    }

    // ===== @property: prompt_token_ids =====
    std::vector<int32_t> prompt_token_ids() const {
        return std::vector<int32_t>(
            token_ids.begin(),
            token_ids.begin() + num_prompt_tokens
        );
    }

    // ===== @property: completion_token_ids =====
    std::vector<int32_t> completion_token_ids() const {
        return std::vector<int32_t>(
            token_ids.begin() + num_prompt_tokens,
            token_ids.end()
        );
    }

    // ===== @property: num_cached_blocks =====
    size_t num_cached_blocks() const {
        return num_cached_tokens / block_size;
    }

    // ===== @property: num_blocks =====
    size_t num_blocks() const {
        return (num_tokens + block_size - 1) / block_size;
    }

    // ===== @property: last_block_num_tokens =====
    size_t last_block_num_tokens() const {
        return num_tokens - (num_blocks() - 1) * block_size;
    }

    // ===== Python: block(i) =====
    std::vector<int32_t> block(size_t i) const {
        assert(i < num_blocks());
        size_t start = i * block_size;
        size_t end   = std::min(start + block_size, num_tokens);
        return std::vector<int32_t>(token_ids.begin() + start,
                                    token_ids.begin() + end);
    }

    // ===== Python: append_token =====
    void append_token(int32_t token_id) {
        token_ids.push_back(token_id);
        last_token = token_id;
        num_tokens++;
    }

    // ===== Python: __getstate__ 等价（序列化用）=====
    struct State {
        size_t num_tokens;
        size_t num_prompt_tokens;
        size_t num_cached_tokens;
        std::vector<int32_t> block_table;

        // 二选一：token_ids 或 last_token
        std::vector<int32_t> token_ids;
        int32_t last_token = -1;
    };

    State get_state() const {
        State s;
        s.num_tokens = num_tokens;
        s.num_prompt_tokens = num_prompt_tokens;
        s.num_cached_tokens = num_cached_tokens;
        s.block_table = block_table;

        if (num_completion_tokens() == 0) {
            s.token_ids = token_ids;
        } else {
            s.last_token = last_token;
        }
        return s;
    }

    // ===== Python: __setstate__ 等价 =====
    void set_state(const State& s) {
        num_tokens = s.num_tokens;
        num_prompt_tokens = s.num_prompt_tokens;
        num_cached_tokens = s.num_cached_tokens;
        block_table = s.block_table;

        if (num_completion_tokens() == 0) {
            token_ids = s.token_ids;
        } else {
            last_token = s.last_token;
        }
    }
};

// ===== 静态变量定义（必须在 cpp 文件里）=====
std::atomic<uint64_t> Sequence::counter{0};
}




}
