#version 460

layout(push_constant) uniform SpriteConstants {
    vec4 rectangle;
    vec2 canvasSize;
} sprite;

layout(location = 0) out vec2 fragTexCoord;

void main() {
    const vec2 corners[6] = vec2[](
        vec2(0, 0), vec2(1, 0), vec2(0, 1),
        vec2(0, 1), vec2(1, 0), vec2(1, 1)
    );

    fragTexCoord = corners[gl_VertexIndex];

    vec2 pixel = sprite.rectangle.xy + fragTexCoord * sprite.rectangle.zw;

    gl_Position = vec4(pixel / sprite.canvasSize * 2.0 - 1.0, 0.0, 1.0);
}
