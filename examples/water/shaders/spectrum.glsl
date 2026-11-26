#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
// spectrum.glsl: the starting amplitudes of every wave, h0(k) and conj(h0(-k)),
// for the three cascades side by side (an atlas 3N wide). Run when the wind changes.
//
// A JONSWAP spectrum (a sea still growing under a wind over a fetch), spread
// around the wind by Donelan-Banner, times Gaussian random numbers
// (Tessendorf). Each cascade keeps its own band of wavenumbers, so together
// they make one sea with no wave counted twice.
layout(location = 0) out vec4 o;
layout(set = 3, binding = 0) uniform Params {
    vec4 uLen;    // the cascades' sizes (m), N
    vec4 uWind;   // direction xy, speed (m/s), fetch (m)
    vec4 uBand;   // band edges between cascades 0|1 and 1|2 (rad/m), shortest-wave damping (m), swell
    vec4 uSeed;   // seed, -, -, -
};

float jonswap(float w, float U, float F) {
    float wp = 22.0 * pow(G * G / (U * F), 1.0 / 3.0);
    float alpha = 0.076 * pow(U * U / (F * G), 0.22);
    float sigma = w <= wp ? 0.07 : 0.09;
    float r = exp(-(w - wp) * (w - wp) / (2.0 * sigma * sigma * wp * wp));
    return alpha * G * G / pow(w, 5.0) * exp(-1.25 * pow(wp / w, 4.0)) * pow(3.3, r);
}

float spreading(float theta, float w, float wp) {
    float ratio = w / wp, beta;
    if (ratio < 0.95) beta = 2.61 * pow(ratio, 1.3);
    else if (ratio < 1.6) beta = 2.28 * pow(ratio, -1.3);
    else beta = pow(10.0, -0.4 + 0.8393 * exp(-0.567 * log(ratio * ratio)));
    float s = 1.0 / cosh(beta * theta);
    return beta / (2.0 * tanh(beta * PI)) * s * s;
}

vec2 gauss(ivec2 mn, int c) {
    uint h = pcg(uint(mn.x) + 1024u * (uint(mn.y) + 1024u * (uint(c) + 4u * uint(uSeed.x))));
    float u1 = u01(h), u2 = u01(pcg(h ^ 0x9e3779b9u));
    float r = sqrt(-2.0 * log(u1));
    return r * vec2(cos(TAU * u2), sin(TAU * u2));
}

vec2 h0(ivec2 mn, int c) {
    int N = int(uLen.w);
    if (mn.x == N / 2 || mn.y == N / 2) return vec2(0.0);   // the Nyquist row has no partner
    float L = c == 0 ? uLen.x : c == 1 ? uLen.y : uLen.z;
    float dk = TAU / L;
    vec2 k = dk * vec2(mn.x < N / 2 ? mn.x : mn.x - N, mn.y < N / 2 ? mn.y : mn.y - N);
    float kl = length(k);
    float lo = c == 0 ? 0.0 : c == 1 ? uBand.x : uBand.y;
    float hi = c == 0 ? uBand.x : c == 1 ? uBand.y : 1e9;
    if (kl < 1e-6 || kl < lo || kl >= hi) return vec2(0.0);
    float U = max(uWind.z, 0.5), F = uWind.w;
    float w = sqrt(G * kl);
    float wp = 22.0 * pow(G * G / (U * F), 1.0 / 3.0);
    float theta = atan(k.y, k.x) - atan(uWind.y, uWind.x);
    theta = mod(theta + PI, TAU) - PI;
    float S = jonswap(w, U, F) * spreading(theta, w, wp);
    // a long swell from a far storm, a little off the wind
    float sw = uBand.w;
    if (sw > 0.0) {
        float wps = 0.42, ths = theta - 0.5;
        ths = mod(ths + PI, TAU) - PI;
        float ss = 0.006 * sw * G * G / pow(w, 5.0) * exp(-1.25 * pow(wps / w, 4.0)) * pow(7.0, exp(-(w - wps) * (w - wps) / (2.0 * 0.0016)));
        S += ss * pow(max(cos(ths * 0.5), 0.0), 40.0) * 3.0;
    }
    float S2 = S * (G / (2.0 * w)) / kl;                // S(w, theta) dw/dk / k: per unit area of k
    S2 *= exp(-kl * kl * uBand.z * uBand.z);            // the shortest ripples die away
    return gauss(mn, c) * 0.5 * sqrt(S2 * dk * dk);
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    int N = int(uLen.w);
    int c = p.x / N;
    ivec2 mn = ivec2(p.x - c * N, p.y);
    ivec2 neg = ivec2((N - mn.x) % N, (N - mn.y) % N);
    o = vec4(h0(mn, c), conj2(h0(neg, c)));
}
