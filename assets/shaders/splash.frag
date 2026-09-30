// DevDesk 启动 splash — 极光波（Aurora）
// EUI-NEO shadertoy 图元方言：mainImage + iTime/iResolution
// 配色：HTML demo 蓝配色；gamma 0.7（深但防黑）；星点适中

float h(vec2 p){return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5);}

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 uv = (fragCoord - 0.5*iResolution.xy)/iResolution.y;
    float t = iTime;
    float w = sin(uv.x*3.0 + t)*0.15 + sin(uv.x*7.0 - t*1.3)*0.08;
    float aur = smoothstep(0.0+w, 0.05+w, uv.y) * smoothstep(0.35+w, 0.2+w, uv.y);
    vec3 deep = vec3(0.02, 0.10, 0.20);
    vec3 wave = vec3(0.06, 0.50, 0.30);
    vec3 high = vec3(0.45, 0.25, 0.95);
    vec3 col = mix(deep, wave, aur);
    col += aur * high * 0.6;
    float st = step(0.998, h(floor(uv*60.0 + t*0.3)));
    col += vec3(1.0)*st*0.5;
    float vig = 1.0 - length(uv)*0.4;
    col *= clamp(vig, 0.5, 1.0);
    col = pow(col, vec3(0.7));
    fragColor = vec4(col, 1.0);
}
