// bolt_optimizer.slang — Adam optimizer for N bolt heights
// P1-L4: Tanh-bounded parameterization — Adam operates on unbounded eps,
// physical heights are h = h_max * tanh(eps). This enforces |h| < h_max
// with no projection, and gradients naturally soften near the boundary.
// P3: Regularization suite — anchor (slope-Gram trust region),
// bend (bending energy), and soft stroke wall.
// When tanhBound==0, skip atanh/tanh and do physical-space Adam directly.
module bolt_optimizer;

static const uint kMaxBolts = 128u;

[[vk::binding(17)]] RWStructuredBuffer<float> boltHeights;
[[vk::binding(13)]] RWStructuredBuffer<float> adamM;
[[vk::binding(14)]] RWStructuredBuffer<float> adamV;
[[vk::binding(18)]] RWStructuredBuffer<float> boltHeightGradient;
// P3: Regularization buffers (read-only)
[[vk::binding(26)]] public uniform StructuredBuffer<float> regGram;        // NB*NB (anchor Gram G)
[[vk::binding(27)]] public uniform StructuredBuffer<float> anchorTarget;   // NB (G@h*)

struct AdamBoltPC {
    float lr;               // offset 0
    float beta1;            // offset 4
    float beta2;            // offset 8
    float eps;              // offset 12
    float iterF;            // offset 16
    uint  numBolts;         // offset 20
    float hMax;             // offset 24 — P1-L4: max bolt stroke (m), tanh bound
    float lambdaStroke;     // offset 28 — P1-L4: stroke regularization weight
    float lambdaAnchor;     // offset 32 — P3: anchor strength
    float lambdaBend;       // offset 36 — P3: bending energy strength
    float lambdaSoft;       // offset 40 — P3: soft stroke wall strength
    uint  tanhBound;        // offset 44 — P3: 1=tanh param, 0=physical space
    float l1Threshold;      // offset 48 — Phase 5.4: L1 soft-threshold per step
                            // (lambda_l1 * lr, physical units). 0 = disabled.
};  // 52 bytes (was 48)

[[vk::push_constant]] AdamBoltPC pc;

// ── helper: sign(x) → -1, 0, or +1 ──
float sign_f(float x) {
    if (x > 0.0f) return 1.0f;
    if (x < 0.0f) return -1.0f;
    return 0.0f;
}

[shader("compute")]
[numthreads(kMaxBolts, 1, 1)]
void adamUpdateBolt(uint3 tid : SV_DispatchThreadID) {
    uint i = tid.x;
    if (i >= pc.numBolts) return;

    // lr==0 (eval mode): skip the whole update. Without this guard the tanh
    // writeback below silently projects out-of-bound init bolts onto
    // 0.999*hMax, corrupting any post-step evaluation.
    if (pc.lr == 0.0f) return;

    float h_b = boltHeights[i];
    float hMax = max(pc.hMax, 1e-6f);

    // ── P3: tanh_bound == 0 → physical-space Adam ──
    if (pc.tanhBound == 0u) {
        float grad_h = boltHeightGradient[i];

        // Anchor regularization (slope-Gram trust region)
        if (pc.lambdaAnchor > 0.0f) {
            float Gh_b = 0.0f;
            for (uint j = 0u; j < pc.numBolts; j++) {
                Gh_b += regGram[i * pc.numBolts + j] * boltHeights[j];
            }
            grad_h += 2.0f * pc.lambdaAnchor * (Gh_b - anchorTarget[i]);
        }

        // Bending energy regularization
        if (pc.lambdaBend > 0.0f) {
            float Kh_b = 0.0f;
            for (uint j = 0u; j < pc.numBolts; j++) {
                Kh_b += regGram[i * pc.numBolts + j] * boltHeights[j];
            }
            grad_h += 2.0f * pc.lambdaBend * Kh_b;
        }

        // Soft stroke wall: quadratic penalty beyond hMax
        if (pc.lambdaSoft > 0.0f) {
            float excess = abs(h_b) - hMax;
            if (excess > 0.0f) {
                grad_h += 2.0f * pc.lambdaSoft * excess * sign_f(h_b);
            }
        }

        // Physical-space Adam update
        float m = pc.beta1 * adamM[i] + (1.0f - pc.beta1) * grad_h;
        float v = pc.beta2 * adamV[i] + (1.0f - pc.beta2) * grad_h * grad_h;
        adamM[i] = m;
        adamV[i] = v;

        float mHat = m / (1.0f - pow(pc.beta1, pc.iterF));
        float vHat = v / (1.0f - pow(pc.beta2, pc.iterF));
        boltHeights[i] -= pc.lr * mHat / (sqrt(vHat) + pc.eps);
        // Phase 5.4: L1 proximal (soft threshold) in physical h space
        if (pc.l1Threshold > 0.0f) {
            float h = boltHeights[i];
            boltHeights[i] = sign_f(h) * max(abs(h) - pc.l1Threshold, 0.0f);
        }
        boltHeightGradient[i] = 0.0f;
        return;
    }

    // ── P1-L4: tanh-bounded parameterization (tanhBound == 1) ──

    // Recover unbounded variable from physical height
    float ratio = clamp(h_b / hMax, -0.999f, 0.999f);
    float eps_b = atanh(ratio);

    // Chain rule: dL/d(eps) = dL/dh * dh/d(eps)
    // dh/d(eps) = hMax * (1 - tanh^2(eps)) = hMax * (1 - ratio^2)
    float dh_deps = hMax * (1.0f - ratio * ratio);
    float grad_eps = boltHeightGradient[i] * dh_deps;

    // P1-L4: optional stroke regularization L_reg = lambda * ||h||^2
    if (pc.lambdaStroke > 0.0f) {
        grad_eps += 2.0f * pc.lambdaStroke * h_b * dh_deps;
    }

    // P3: Anchor regularization: grad += 2*lambda_s * ((G@h)_b - (G@h*)_b) * dh_deps
    if (pc.lambdaAnchor > 0.0f) {
        float Gh_b = 0.0f;
        for (uint j = 0u; j < pc.numBolts; j++) {
            Gh_b += regGram[i * pc.numBolts + j] * boltHeights[j];
        }
        grad_eps += 2.0f * pc.lambdaAnchor * (Gh_b - anchorTarget[i]) * dh_deps;
    }

    // P3: Bending energy regularization
    if (pc.lambdaBend > 0.0f) {
        float Kh_b = 0.0f;
        for (uint j = 0u; j < pc.numBolts; j++) {
            Kh_b += regGram[i * pc.numBolts + j] * boltHeights[j];
        }
        grad_eps += 2.0f * pc.lambdaBend * Kh_b * dh_deps;
    }

    // P3: Soft stroke wall
    if (pc.lambdaSoft > 0.0f) {
        float excess = abs(h_b) - hMax;
        if (excess > 0.0f) {
            grad_eps += 2.0f * pc.lambdaSoft * excess * sign_f(h_b) * dh_deps;
        }
    }

    // Adam update in epsilon space (unbounded)
    float m = pc.beta1 * adamM[i] + (1.0f - pc.beta1) * grad_eps;
    float v = pc.beta2 * adamV[i] + (1.0f - pc.beta2) * grad_eps * grad_eps;
    adamM[i] = m;
    adamV[i] = v;

    float mHat = m / (1.0f - pow(pc.beta1, pc.iterF));
    float vHat = v / (1.0f - pow(pc.beta2, pc.iterF));
    eps_b -= pc.lr * mHat / (sqrt(vHat) + pc.eps);

    // Map back to physical space
    float h_new = hMax * tanh(eps_b);
    // Phase 5.4: L1 proximal (soft threshold) in physical h space. The next
    // iteration recovers eps from this thresholded h via atanh(clamp(...)),
    // so the proximal step is self-consistent with the tanh parameterization.
    if (pc.l1Threshold > 0.0f) {
        h_new = sign_f(h_new) * max(abs(h_new) - pc.l1Threshold, 0.0f);
    }
    boltHeights[i] = h_new;
    boltHeightGradient[i] = 0.0f;
}
