#version 450
// aurora.glsl: the whole picture in one pass. Four soft blobs of light over a
// night blue (the page's blurred radial gradients, laid in order like its
// divs), a glow under the cursor, twinkling stars (one per grid cell, the
// page's density) and a shooting star.
layout(location = 0) out vec4 oColor;
layout(set = 3, binding = 0) uniform Params {
    vec4 uRes;        // width, height (px), pixel scale, time (s)
    vec4 uBlob[4];    // centre x, y (px), radius (px), opacity
    vec4 uCol[4];     // colour, softness (px)
    vec4 uBg;         // background, brightness
    vec4 uStars;      // cell (px), 0..1 star chance, twinkle 0..1, star size scale
    vec4 uCursor;     // x, y (px), strength 0..1, radius (px)
    vec4 uCursorCol;  // colour
    vec4 uShoot;      // head x, y (px), direction (radians), brightness 0..1 (0 = none)
    vec4 uShootLen;   // tail length (px), width (px)
};

float hash12(vec2 p) { vec3 p3 = fract(vec3(p.xyx) * .1031); p3 += dot(p3, p3.yzx + 33.33); return fract((p3.x + p3.y) * p3.z); }
vec2 hash22(vec2 p) { vec3 p3 = fract(vec3(p.xyx) * vec3(.1031, .1030, .0973)); p3 += dot(p3, p3.yzx + 33.33); return fract((p3.xx + p3.yz) * p3.zy); }

// A radial gradient from full colour at the centre to nothing at its radius,
// blurred: the cone's kink at the rim and its peak are rounded off.
float blob(vec2 p, vec4 b, float soft) {
    float x = (b.z - length(p - b.xy)) / b.z;            // 1 at the centre, 0 at the rim
    float k = soft / b.z;
    float a = .5 * (x + sqrt(x * x + k * k));            // a soft max(x, 0): the blur spills past the rim
    return clamp(a, 0., 1.) * b.w;
}

void main() {
    vec2 p = gl_FragCoord.xy;
    float t = uRes.w;
    vec3 col = uBg.rgb;
    for (int i = 0; i < 4; i++) col = mix(col, uCol[i].rgb, blob(p, uBlob[i], uCol[i].w));

    // the cursor's glow: light gathered where the pointer rests
    if (uCursor.z > 0.) {
        float d = length(p - uCursor.xy) / uCursor.w;
        col += uCursorCol.rgb * uCursor.z * exp(-d * d * 2.2) * .22;
    }
    col *= uBg.w;

    // stars: one per cell at most, kept off the cell's edge so only this cell can cover the pixel
    if (uStars.y > 0.) {
        vec2 cell = floor(p / uStars.x);
        float h = hash12(cell + 17.);
        if (h < uStars.y) {
            vec2 r3 = hash22(cell);
            float s = uStars.w * uRes.z;
            float rad = (hash12(cell + 3.1) * 1.2 + .3) * s;
            vec2 at = (cell + .1 + .8 * r3) * uStars.x;
            float cover = clamp(rad - length(p - at) + .5, 0., 1.);
            if (cover > 0.) {
                float ph = hash12(cell + 9.7) * 6.2832, sp = .4 + hash12(cell + 5.3);
                float a = mix(.8, .25 + .55 * (.5 + .5 * sin(ph + t * sp)), uStars.z);
                col = mix(col, vec3(1), cover * a);
            }
        }
    }

    // shooting star: a bright head and a tail that thins and fades behind it
    if (uShoot.w > 0.) {
        vec2 dir = vec2(cos(uShoot.z), sin(uShoot.z));
        vec2 q = p - uShoot.xy;
        float along = -dot(q, dir);                           // distance behind the head
        float across = abs(dot(q, vec2(-dir.y, dir.x)));
        float f = clamp(1. - along / uShootLen.x, 0., 1.) * step(-uShootLen.y, along);
        float w = uShootLen.y * (.3 + .7 * f);
        float line = clamp(w - across + .5, 0., 1.) * f * f;
        float head = exp(-dot(q, q) / (uShootLen.y * uShootLen.y * 6.));
        col += vec3(.9, .95, 1.) * (line * 1.6 + head) * uShoot.w;
    }

    col += (hash12(p + fract(t) * 61.) - .5) / 255.;          // dither: no banding in the dark gradients
    oColor = vec4(col, 1.);
}
