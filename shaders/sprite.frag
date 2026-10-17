#version 450

layout(set = 0, binding = 0) uniform sampler2D albedoMap;
layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 fragColor;
layout(constant_id = 0) const bool srgbAttachment = false;

void main() {
    vec4 texColor = texture(albedoMap, fragTexCoord);
    if (texColor.a < 0.001) discard;
    fragColor = texColor;
    // Undo the attachment's sRGB encoding to retain the OpenGL version's colors.
    if (srgbAttachment) {
        fragColor.rgb = mix(texColor.rgb / 12.92,
                           pow((texColor.rgb + 0.055) / 1.055, vec3(2.4)),
                           greaterThan(texColor.rgb, vec3(0.04045)));
    }
}
