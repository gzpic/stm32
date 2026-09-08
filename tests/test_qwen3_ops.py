import dataclasses
from typing import List, Optional

import pytest
import torch


@dataclasses.dataclass
class CaseSpec:
    name: str
    batch: int
    seq_len: int
    hidden: int
    num_heads: int
    num_kv_heads: int
    head_dim: int
    ffn_hidden: int
    max_seq_len: int
    position: int
    position_offset: int
    rope_theta: float
    epsilon: float
    dtype: torch.dtype
    seed: int
    abs_eps: float
    rel_eps: float


def build_cases() -> List[CaseSpec]:
    return [
        CaseSpec(
            name="typical_small",
            batch=2,
            seq_len=7,
            hidden=64,
            num_heads=4,
            num_kv_heads=4,
            head_dim=16,
            ffn_hidden=128,
            max_seq_len=128,
            position=3,
            position_offset=0,
            rope_theta=10000.0,
            epsilon=1e-5,
            dtype=torch.float16,
            seed=1234,
            abs_eps=1e-2,
            rel_eps=1e-2,
        ),
        CaseSpec(
            name="typical_mid",
            batch=1,
            seq_len=128,
            hidden=128,
            num_heads=8,
            num_kv_heads=8,
            head_dim=16,
            ffn_hidden=256,
            max_seq_len=256,
            position=127,
            position_offset=0,
            rope_theta=10000.0,
            epsilon=1e-5,
            dtype=torch.float16,
            seed=4321,
            abs_eps=1e-2,
            rel_eps=1e-2,
        ),
        CaseSpec(
            name="unaligned_dims",
            batch=1,
            seq_len=7,
            hidden=33,
            num_heads=1,
            num_kv_heads=1,
            head_dim=33,
            ffn_hidden=17,
            max_seq_len=64,
            position=5,
            position_offset=0,
            rope_theta=10000.0,
            epsilon=1e-5,
            dtype=torch.float16,
            seed=999,
            abs_eps=2e-2,
            rel_eps=2e-2,
        ),
        CaseSpec(
            name="tiny",
            batch=1,
            seq_len=1,
            hidden=1,
            num_heads=1,
            num_kv_heads=1,
            head_dim=1,
            ffn_hidden=1,
            max_seq_len=1,
            position=0,
            position_offset=0,
            rope_theta=10000.0,
            epsilon=1e-5,
            dtype=torch.float16,
            seed=0,
            abs_eps=1e-3,
            rel_eps=1e-3,
        ),
        CaseSpec(
            name="degenerate_rope_pos0",
            batch=1,
            seq_len=1,
            hidden=32,
            num_heads=2,
            num_kv_heads=2,
            head_dim=16,
            ffn_hidden=64,
            max_seq_len=32,
            position=0,
            position_offset=0,
            rope_theta=10000.0,
            epsilon=1e-5,
            dtype=torch.float16,
            seed=42,
            abs_eps=1e-3,
            rel_eps=1e-3,
        ),
    ]


def ref_embedding(input_ids: torch.Tensor, embedding: torch.Tensor) -> torch.Tensor:
    return embedding[input_ids]


def ref_rmsnorm(x: torch.Tensor, gamma: torch.Tensor, eps: float) -> torch.Tensor:
    rms = torch.sqrt(torch.mean(x * x, dim=-1, keepdim=True) + eps)
    return x / rms * gamma


def apply_rope(x: torch.Tensor, position: int, rope_theta: float, rotary_dim: Optional[int] = None) -> torch.Tensor:
    head_dim = x.shape[-1]
    if rotary_dim is None:
        rotary_dim = head_dim
    out = x.clone()
    for i in range(0, rotary_dim - 1, 2):
        inv_freq = rope_theta ** (-2 * (i // 2) / rotary_dim)
        angle = position * inv_freq
        c = torch.cos(angle)
        s = torch.sin(angle)
        x0 = x[..., i]
        x1 = x[..., i + 1]
        out[..., i] = x0 * c - x1 * s
        out[..., i + 1] = x0 * s + x1 * c
    return out


def ref_attention(q: torch.Tensor, k: torch.Tensor, v: torch.Tensor) -> torch.Tensor:
    scores = torch.matmul(q, k.transpose(-1, -2))
    weights = torch.softmax(scores, dim=-1)
    return torch.matmul(weights, v)


def ref_swiglu(x: torch.Tensor, w1: torch.Tensor, w2: torch.Tensor, w3: torch.Tensor) -> torch.Tensor:
    gate = torch.matmul(x, w1)
    up = torch.matmul(x, w2)
    y = torch.nn.functional.silu(gate) * up
    return torch.matmul(y, w3)


def assert_close(out_ref: torch.Tensor, out_cuda: torch.Tensor, abs_eps: float, rel_eps: float) -> None:
    diff = torch.abs(out_ref - out_cuda)
    max_abs = diff.max().item()
    max_rel = (diff / (out_ref.abs() + 1e-6)).max().item()
    assert max_abs < abs_eps, f"max_abs {max_abs} > {abs_eps}"
    assert max_rel < rel_eps, f"max_rel {max_rel} > {rel_eps}"


def load_extension():
    try:
        import qwen3_ops  # type: ignore
        return qwen3_ops
    except Exception:
        return None


@pytest.mark.parametrize("case", build_cases())
@pytest.mark.skipif(not torch.cuda.is_available(), reason="CUDA required")
def test_embedding(case: CaseSpec):
    ext = load_extension()
    if ext is None:
        pytest.skip("qwen3_ops extension not available")

    torch.manual_seed(case.seed)
    vocab = 128
    input_ids = torch.randint(0, vocab, (case.batch, case.seq_len), dtype=torch.int32)
    embedding = torch.randn(vocab, case.hidden, dtype=torch.float32)

    ref = ref_embedding(input_ids, embedding)

    input_ids_cuda = input_ids.cuda()
    embedding_cuda = embedding.cuda().half()
    output_cuda = torch.empty_like(ref, device="cuda", dtype=torch.float16)

    ext.embedding_forward(input_ids_cuda, embedding_cuda, output_cuda, case.batch, case.seq_len, case.hidden, stage="prefill")

    out = output_cuda.float().cpu()
    assert torch.equal(out.view(torch.uint16), ref.half().view(torch.uint16))


@pytest.mark.parametrize("case", build_cases())
@pytest.mark.skipif(not torch.cuda.is_available(), reason="CUDA required")
def test_rmsnorm_qkv(case: CaseSpec):
    ext = load_extension()
    if ext is None:
        pytest.skip("qwen3_ops extension not available")

    torch.manual_seed(case.seed)
    x = torch.randn(case.batch, case.seq_len, case.hidden, dtype=torch.float32)
    gamma = torch.randn(case.hidden, dtype=torch.float32)
    w_qkv = torch.randn(case.hidden, case.hidden * 3, dtype=torch.float32)

    ref_norm = ref_rmsnorm(x, gamma, case.epsilon)
    ref_qkv = torch.matmul(ref_norm, w_qkv)

    x_cuda = x.cuda().half()
    gamma_cuda = gamma.cuda().half()
    w_qkv_cuda = w_qkv.cuda().half()
    qkv_cuda = torch.empty(case.batch, case.seq_len, case.hidden * 3, device="cuda", dtype=torch.float16)
    rms_out = torch.empty(case.batch, case.seq_len, case.hidden, device="cuda", dtype=torch.float16)

    ext.rms_qkv_forward(x_cuda, w_qkv_cuda, gamma_cuda, qkv_cuda, rms_out, case.epsilon, case.batch, case.seq_len, case.hidden, stage="prefill")

    out = qkv_cuda.float().cpu()
    assert_close(ref_qkv, out, case.abs_eps, case.rel_eps)


@pytest.mark.parametrize("case", build_cases())
@pytest.mark.skipif(not torch.cuda.is_available(), reason="CUDA required")
def test_qk_norm_rope(case: CaseSpec):
    ext = load_extension()
    if ext is None:
        pytest.skip("qwen3_ops extension not available")

    if case.head_dim % 2 != 0:
        pytest.skip("rotary dim requires even head_dim")

    torch.manual_seed(case.seed)
    x = torch.randn(case.batch, case.seq_len, case.hidden, dtype=torch.float32)
    gamma = torch.randn(case.hidden, dtype=torch.float32)

    x_heads = x.view(case.batch, case.seq_len, case.num_heads, case.head_dim)
    gamma_heads = gamma.view(case.num_heads, case.head_dim)
    ref_out = []
    for b in range(case.batch):
        token_out = []
        for t in range(case.seq_len):
            head_out = []
            for h in range(case.num_heads):
                norm = ref_rmsnorm(x_heads[b, t, h], gamma_heads[h], case.epsilon)
                rope = apply_rope(norm, t + case.position_offset, case.rope_theta)
                head_out.append(rope)
            token_out.append(torch.stack(head_out, dim=0))
        ref_out.append(torch.stack(token_out, dim=0))
    ref_out = torch.stack(ref_out, dim=0)
    ref_out = ref_out.view(case.batch, case.seq_len, case.hidden)

    max_seq = case.max_seq_len
    rotary_dim = case.head_dim
    positions = torch.arange(0, max_seq, dtype=torch.float32)
    inv_freq = case.rope_theta ** (-2 * torch.arange(0, rotary_dim, 2, dtype=torch.float32) / rotary_dim)
    freqs = torch.outer(positions, inv_freq)
    cos_table = torch.cos(freqs)
    sin_table = torch.sin(freqs)

    x_cuda = x.cuda().half()
    gamma_cuda = gamma.cuda().half()
    out_cuda = torch.empty_like(x_cuda)
    cos_cuda = cos_table.cuda()
    sin_cuda = sin_table.cuda()

    ext.qk_norm_rope_forward(
        x_cuda,
        gamma_cuda,
        out_cuda,
        case.epsilon,
        case.batch,
        case.seq_len,
        case.hidden,
        case.num_heads,
        0,
        case.head_dim,
        rotary_dim,
        max_seq,
        cos_cuda,
        sin_cuda,
        stage="prefill",
    )

    out = out_cuda.float().cpu()
    assert_close(ref_out, out, case.abs_eps, case.rel_eps)

    if case.seq_len == 1:
        out_prefill = out.clone()
        ext.qk_norm_rope_forward(
            x_cuda,
            gamma_cuda,
            out_cuda,
            case.epsilon,
            case.batch,
            1,
            case.hidden,
            case.num_heads,
            case.position,
            case.head_dim,
            rotary_dim,
            max_seq,
            cos_cuda,
            sin_cuda,
            stage="decode",
        )
        out_decode = out_cuda.float().cpu()
        assert_close(out_prefill, out_decode, case.abs_eps, case.rel_eps)


@pytest.mark.parametrize("case", build_cases())
@pytest.mark.skipif(not torch.cuda.is_available(), reason="CUDA required")
def test_ffn_swiglu(case: CaseSpec):
    ext = load_extension()
    if ext is None:
        pytest.skip("qwen3_ops extension not available")

    torch.manual_seed(case.seed)
    x = torch.randn(case.batch, case.seq_len, case.hidden, dtype=torch.float32)
    w1 = torch.randn(case.hidden, case.ffn_hidden, dtype=torch.float32)
    w2 = torch.randn(case.hidden, case.ffn_hidden, dtype=torch.float32)
    w3 = torch.randn(case.ffn_hidden, case.hidden, dtype=torch.float32)

    ref = ref_swiglu(x, w1, w2, w3)

    x_cuda = x.cuda().half()
    w1_cuda = w1.cuda().half()
    w2_cuda = w2.cuda().half()
    w3_cuda = w3.cuda().half()

    packed_w1 = torch.empty(case.hidden, case.ffn_hidden * 2, device="cuda", dtype=torch.float16)
    ext.ffn_pack_w1(w1_cuda, w2_cuda, packed_w1, case.hidden, case.ffn_hidden)

    workspace_elems = case.batch * case.seq_len * case.ffn_hidden * 3
    workspace = torch.empty(workspace_elems, device="cuda", dtype=torch.float16)
    out_cuda = torch.empty(case.batch, case.seq_len, case.hidden, device="cuda", dtype=torch.float16)

    ext.ffn_swiglu_forward(
        x_cuda,
        w1_cuda,
        w2_cuda,
        w3_cuda,
        packed_w1,
        out_cuda,
        workspace,
        workspace.numel() * workspace.element_size(),
        case.batch,
        case.seq_len,
        case.hidden,
        case.ffn_hidden,
        stage="prefill",
    )

    out = out_cuda.float().cpu()
    assert_close(ref, out, case.abs_eps, case.rel_eps)


@pytest.mark.parametrize("case", build_cases())
@pytest.mark.skipif(not torch.cuda.is_available(), reason="CUDA required")
def test_kv_cache_write(case: CaseSpec):
    ext = load_extension()
    if ext is None:
        pytest.skip("qwen3_ops extension not available")

    if case.head_dim % 2 != 0:
        pytest.skip("rotary dim requires even head_dim")

    torch.manual_seed(case.seed)
    key = torch.randn(case.batch, case.num_kv_heads * case.head_dim, dtype=torch.float32)
    value = torch.randn(case.batch, case.num_kv_heads * case.head_dim, dtype=torch.float32)
    gamma = torch.randn(case.num_kv_heads * case.head_dim, dtype=torch.float32)

    rotary_dim = case.head_dim
    positions = torch.arange(0, case.max_seq_len, dtype=torch.float32)
    inv_freq = case.rope_theta ** (-2 * torch.arange(0, rotary_dim, 2, dtype=torch.float32) / rotary_dim)
    freqs = torch.outer(positions, inv_freq)
    cos_table = torch.cos(freqs)
    sin_table = torch.sin(freqs)

    key_cuda = key.cuda().half()
    value_cuda = value.cuda().half()
    gamma_cuda = gamma.cuda().half()
    cos_cuda = cos_table.cuda()
    sin_cuda = sin_table.cuda()

    head_dim = case.head_dim
    stride_tokens = head_dim
    stride_heads = case.max_seq_len * head_dim
    stride_batch = case.num_kv_heads * stride_heads

    key_cache = torch.full(
        (case.batch, case.num_kv_heads, case.max_seq_len, head_dim),
        fill_value=0xCD,
        device="cuda",
        dtype=torch.float16,
    )
    value_cache = torch.full(
        (case.batch, case.num_kv_heads, case.max_seq_len, head_dim),
        fill_value=0xCD,
        device="cuda",
        dtype=torch.float16,
    )
    key_out = torch.empty_like(key_cuda)

    ext.k_norm_rope_kvcache_decode(
        key_cuda,
        value_cuda,
        gamma_cuda,
        key_out,
        key_cache,
        value_cache,
        case.position,
        case.epsilon,
        case.batch,
        case.num_kv_heads,
        case.head_dim,
        case.head_dim,
        case.max_seq_len,
        stride_tokens,
        stride_heads,
        stride_batch,
        cos_cuda,
        sin_cuda,
    )

    key_cache_cpu = key_cache.cpu().float()
    value_cache_cpu = value_cache.cpu().float()

    mask = torch.ones_like(key_cache_cpu, dtype=torch.bool)
    mask[:, :, case.position, :] = False
    assert torch.equal(key_cache_cpu[mask], torch.full_like(key_cache_cpu[mask], float(0xCD)))
    assert torch.equal(value_cache_cpu[mask], torch.full_like(value_cache_cpu[mask], float(0xCD)))


@pytest.mark.parametrize("case", build_cases())
@pytest.mark.skipif(not torch.cuda.is_available(), reason="CUDA required")
def test_attention_ref(case: CaseSpec):
    torch.manual_seed(case.seed)
    q = torch.randn(case.batch, case.num_heads, case.seq_len, case.head_dim, dtype=torch.float32)
    k = torch.randn(case.batch, case.num_heads, case.seq_len, case.head_dim, dtype=torch.float32)
    v = torch.randn(case.batch, case.num_heads, case.seq_len, case.head_dim, dtype=torch.float32)
    out = ref_attention(q, k, v)
    assert out.shape == q.shape
