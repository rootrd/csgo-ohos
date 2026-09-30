// The rigid, opaque DepthWrite transform. The model rows come from stream 1
// instead of c58..c60; c8..c11 remain the original view/projection constants.
// No CPU vertex transforms, texture samples, bone blending or tree sway.
float4 viewProjection[4] : register(c8);

struct Input
{
    float4 position : POSITION0;
    float4 model0 : TEXCOORD4;
    float4 model1 : TEXCOORD5;
    float4 model2 : TEXCOORD6;
};

float4 main(Input input) : POSITION0
{
    float4 world = float4(dot(input.position, input.model0),
                          dot(input.position, input.model1),
                          dot(input.position, input.model2), 1.0);
    return float4(dot(world, viewProjection[0]),
                  dot(world, viewProjection[1]),
                  dot(world, viewProjection[2]),
                  dot(world, viewProjection[3]));
}
