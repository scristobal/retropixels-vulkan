#version 450

layout(location = 0) in vec3 position;
layout(location = 1) in vec2 texCoord;
layout(location = 2) in mat4 modelMatrix;

layout(push_constant) uniform SpriteConstants {
    mat4 texTransform;
} sprite;

layout(location = 0) out vec2 fragTexCoord;

void main() {
    gl_Position = modelMatrix * vec4(position, 1.0);
    // Vulkan's positive-height viewport has its origin at the top left.
    gl_Position.y = -gl_Position.y;
    fragTexCoord = (sprite.texTransform * vec4(texCoord, 0.0, 1.0)).xy;
}
