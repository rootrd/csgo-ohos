#ifndef SOURCE_VULKAN_ALPHA_HLSL
#define SOURCE_VULKAN_ALPHA_HLSL
// Vulkan compare-op numbering, matching SourceAlphaState. Source's shadow API
// truncates alphaRef to an 8-bit value before storing reference = alphaRef / 255.
struct SourceAlphaState
{
    uint enabled;
    uint compare;
    float reference;
    uint reserved;
};
void SourceAlphaTest(float alpha, SourceAlphaState state)
{
    if (!state.enabled) return;
    bool passes = false;
    switch (state.compare)
    {
        case 0: passes = false; break;
        case 1: passes = alpha < state.reference; break;
        case 2: passes = alpha == state.reference; break;
        case 3: passes = alpha <= state.reference; break;
        case 4: passes = alpha > state.reference; break;
        case 5: passes = alpha != state.reference; break;
        case 6: passes = alpha >= state.reference; break;
        case 7: passes = true; break;
    }
    if (!passes) discard;
}
#endif
