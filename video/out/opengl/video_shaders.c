/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with mpv.  If not, see <http://www.gnu.org/licenses/>.
 *
 * You can alternatively redistribute this file and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <math.h>

#include "video_shaders.h"
#include "video.h"

#define GLSL(x) gl_sc_add(sc, #x "\n");
#define GLSLF(...) gl_sc_addf(sc, __VA_ARGS__)
#define GLSLH(x) gl_sc_hadd(sc, #x "\n");
#define GLSLHF(...) gl_sc_haddf(sc, __VA_ARGS__)

// Defines a colorspace-dependent macro to obtain a sample's luminance, works
// with both linear and companded RGB
static void luma_header(struct gl_shader_cache *sc, enum mp_csp_prim primaries)
{
    // Obtain the luma coefficients from the RGB->XYZ matrix's Y column
    float m[3][3];
    struct mp_csp_primaries csp = mp_get_csp_primaries(primaries);
    mp_get_rgb2xyz_matrix(csp, m);
    GLSLHF("#define luma(v) dot(vec3(%f, %f, %f), v.rgb)\n",
                m[1][0], m[1][1], m[1][2]);
}

void super_xbr_header(struct gl_shader_cache *sc, int pass,
                      enum mp_csp_prim primaries, float weight, float edge_str)
{
    // Sample from the appropriately rotated plane. This could possibly
    // be done in a better way by rotating the actual source coordinates,
    // but I'd rather the algorithm work first. Also set up the weights.
    if (pass == 0) {
        GLSLH(#define get(x, y) texture(tex, pos + pt * (vec2(x,y) - vec2(0.25))).rgb)

        GLSLH(#define wp1  2.0)
        GLSLH(#define wp2  1.0)
        GLSLH(#define wp3 -1.0)
        GLSLH(#define wp4  4.0)
        GLSLH(#define wp5 -1.0)
        GLSLH(#define wp6  1.0)

        GLSLHF("#define weight1 (%f*0.129633)\n", weight);
        GLSLHF("#define weight2 (%f*0.175068)\n", weight/2.0);

    } else {
        GLSLH(#define get(x, y) texture(tex, pos + pt * vec2((x)+(y)-1, (y)-(x))).rgb)

        GLSLH(#define wp1 2.0)
        GLSLH(#define wp2 0.0)
        GLSLH(#define wp3 0.0)
        GLSLH(#define wp4 0.0)
        GLSLH(#define wp5 0.0)
        GLSLH(#define wp6 0.0)

        GLSLHF("#define weight1 (%f*0.175068)\n", weight);
        GLSLHF("#define weight2 (%f*0.129633)\n", weight/2.0);
    }

    // Weight function helpers
    GLSLH(#define d(a,b) distance(a,b))
    GLSLH(float d_wd(float b0, float b1, float c0, float c1, float c2,
                     float d0, float d1, float d2, float d3, float e1,
                     float e2, float e3, float f2, float f3)
          {
              return wp1*(d(c1,c2) + d(c1,c0) + d(e2,e1) + d(e2,e3))
                   + wp2*(d(d2,d3) + d(d0,d1))
                   + wp3*(d(d1,d3) + d(d0,d2))
                   + wp4*d(d1,d2)
                   + wp5*(d(c0,c2) + d(e1,e3))
                   + wp6*(d(b0,b1) + d(f2,f3));
          }

          float o_wd(float i1, float i2, float i3, float i4,
                     float e1, float e2, float e3, float e4)
          {
              return wp4*(d(i1,i2) + d(i3,i4))
                   + wp1*(d(i1,e1) + d(i2,e2) + d(i3,e3) + d(i4,e4))
                   + wp3*(d(i1,e2) + d(i3,e4) + d(e1,i2) + d(e3,i4));
          }
    )

    // The shader logic is inside a sub-function to let us return prematurely
    GLSLHF("vec3 super_xbr(sampler2D tex, vec2 pos, vec2 size) {\n");

    // Return untouched the pixels that form part of the original image
    GLSLH(vec2 pt = vec2(1.0) / size;)
    GLSLHF("vec2 dir = fract(pos * size / %f) - 0.5;\n", pass+1.0);
    if (pass == 0) {
        // Optimization: Discard (skip drawing) unused pixels, except those
        // at the edge.
        GLSLH(vec2 dist = size * min(pos, vec2(1.0) - pos);)
        GLSLH(if (dir.x * dir.y < 0 && dist.x > 1 && dist.y > 1)
                  return vec3(0.0);)
        GLSLH(if (dir.x < 0 || dir.y < 0 || dist.x < 1 || dist.y < 1)
                  return texture(tex, pos - pt * dir).rgb;)
    } else {
        GLSLH(if (dir.x * dir.y > 0) return texture(tex, pos).rgb;)
    }

    // Sample all the necessary pixels
    GLSLH(vec3 P0 = get(-1,-1);)
    GLSLH(vec3 P1 = get( 2,-1);)
    GLSLH(vec3 P2 = get(-1, 2);)
    GLSLH(vec3 P3 = get( 2, 2);)

    GLSLH(vec3 B = get( 0,-1);)
    GLSLH(vec3 C = get( 1,-1);)
    GLSLH(vec3 D = get(-1, 0);)
    GLSLH(vec3 E = get( 0, 0);)
    GLSLH(vec3 F = get( 1, 0);)
    GLSLH(vec3 G = get(-1, 1);)
    GLSLH(vec3 H = get( 0, 1);)
    GLSLH(vec3 I = get( 1, 1);)

    GLSLH(vec3 F4 = get(2, 0);)
    GLSLH(vec3 I4 = get(2, 1);)
    GLSLH(vec3 H5 = get(0, 2);)
    GLSLH(vec3 I5 = get(1, 2);)

    // Get their corresponding brightness values
    luma_header(sc, primaries);
    GLSLH(float b = luma(B);)
    GLSLH(float c = luma(C);)
    GLSLH(float d = luma(D);)
    GLSLH(float e = luma(E);)
    GLSLH(float f = luma(F);)
    GLSLH(float g = luma(G);)
    GLSLH(float h = luma(H);)
    GLSLH(float i = luma(I);)

    GLSLH(float i4 = luma(I4); float p0 = luma(P0);)
    GLSLH(float i5 = luma(I5); float p1 = luma(P1);)
    GLSLH(float h5 = luma(H5); float p2 = luma(P2);)
    GLSLH(float f4 = luma(F4); float p3 = luma(P3);)

    /*
                                  P1
         |P0|B |C |P1|         C     F4          |a0|b1|c2|d3|
         |D |E |F |F4|      B     F     I4       |b0|c1|d2|e3|   |e1|i1|i2|e2|
         |G |H |I |I4|   P0    E  A  I     P3    |c0|d1|e2|f3|   |e3|i3|i4|e4|
         |P2|H5|I5|P3|      D     H     I5       |d0|e1|f2|g3|
                               G     H5
                                  P2
    */

    // Compute edge coefficients in the diagonal and orthogonal directions
    GLSLH(float d_edge = (d_wd(d, b, g, e, c, p2, h, f, p1, h5, i, f4, i5, i4)
                        - d_wd(c, f4, b, f, i4, p0, e, i, p3, d, h, i5, g, h5));)
    GLSLH(float o_edge = (o_wd(f, i, e, h, c, i5, b, h5)
                        - o_wd(e, f, h, i, d, f4, g, i4));)

    // Weight vectors for filtering (two taps)
    GLSLH(vec4 w1 = vec4(-weight1, vec2(weight1+0.50), -weight1);)
    GLSLH(vec4 w2 = vec4(-weight2, vec2(weight2+0.25), -weight2);)

    // Filtering and normalization in four directions
    GLSLH(vec3 c1 = mat4x3(P2, H, F, P1) * w1;)
    GLSLH(vec3 c2 = mat4x3(P0, E, I, P3) * w1;)
    GLSLH(vec3 c3 = mat4x3( D, E, F, F4) * w2 + mat4x3( G, H, I, I4) * w2;)
    GLSLH(vec3 c4 = mat4x3( C, F, I, I5) * w2 + mat4x3( B, E, H, H5) * w2;)

    // Generate the output color by smoothly blending the two strongest dirs
    GLSLHF("float edge_str = smoothstep(0.0, %f + 1e-6, abs(d_edge));\n",
                edge_str);
    GLSLH(vec3 color = mix(mix(c1, c2, step(0.0, d_edge)),
                           mix(c3, c4, step(0.0, o_edge)),
                           1 - edge_str);)

    // Simple anti-ringing code
    GLSLH(vec3 lo = min(min(E, F), min(H, I));)
    GLSLH(vec3 hi = max(max(E, F), max(H, I));)
    GLSLH(vec3 clamped = clamp(color, lo, hi);)
    GLSLH(color = mix(color, clamped, 1.0 - 2.0 * abs(edge_str - 0.5));)

    GLSLH(return color;)
    GLSLHF("}\n");
}

// Set up shared/commonly used variables
void sampler_prelude(struct gl_shader_cache *sc, int tex_num)
{
    GLSLF("#undef tex\n");
    GLSLF("#define tex texture%d\n", tex_num);
    GLSLF("vec2 pos = texcoord%d;\n", tex_num);
    GLSLF("vec2 size = texture_size%d;\n", tex_num);
    GLSLF("vec2 pt = vec2(1.0) / size;\n");
}

static void pass_sample_separated_get_weights(struct gl_shader_cache *sc,
                                              struct scaler *scaler)
{
    gl_sc_uniform_sampler(sc, "lut", scaler->gl_target,
                          TEXUNIT_SCALERS + scaler->index);

    int N = scaler->kernel->size;
    if (N == 2) {
        GLSL(vec2 c1 = texture(lut, vec2(0.5, fcoord)).RG;)
        GLSL(float weights[2] = float[](c1.r, c1.g);)
    } else if (N == 6) {
        GLSL(vec4 c1 = texture(lut, vec2(0.25, fcoord));)
        GLSL(vec4 c2 = texture(lut, vec2(0.75, fcoord));)
        GLSL(float weights[6] = float[](c1.r, c1.g, c1.b, c2.r, c2.g, c2.b);)
    } else {
        GLSLF("float weights[%d];\n", N);
        for (int n = 0; n < N / 4; n++) {
            GLSLF("c = texture(lut, vec2(1.0 / %d + %d / float(%d), fcoord));\n",
                    N / 2, n, N / 4);
            GLSLF("weights[%d] = c.r;\n", n * 4 + 0);
            GLSLF("weights[%d] = c.g;\n", n * 4 + 1);
            GLSLF("weights[%d] = c.b;\n", n * 4 + 2);
            GLSLF("weights[%d] = c.a;\n", n * 4 + 3);
        }
    }
}

// Handle a single pass (either vertical or horizontal). The direction is given
// by the vector (d_x, d_y). If the vector is 0, then planar interpolation is
// used instead (samples from texture0 through textureN)
void pass_sample_separated_gen(struct gl_shader_cache *sc, struct scaler *scaler,
                               int d_x, int d_y)
{
    int N = scaler->kernel->size;
    bool use_ar = scaler->conf.antiring > 0;
    bool planar = d_x == 0 && d_y == 0;
    GLSL(vec4 color = vec4(0.0);)
    GLSLF("{\n");
    if (!planar) {
        GLSLF("vec2 dir = vec2(%d, %d);\n", d_x, d_y);
        GLSL(pt *= dir;)
        GLSL(float fcoord = dot(fract(pos * size - vec2(0.5)), dir);)
        GLSLF("vec2 base = pos - fcoord * pt - pt * vec2(%d);\n", N / 2 - 1);
    }
    GLSL(vec4 c;)
    if (use_ar) {
        GLSL(vec4 hi = vec4(0.0);)
        GLSL(vec4 lo = vec4(1.0);)
    }
    pass_sample_separated_get_weights(sc, scaler);
    GLSLF("// scaler samples\n");
    for (int n = 0; n < N; n++) {
        if (planar) {
            GLSLF("c = texture(texture%d, texcoord%d);\n", n, n);
        } else {
            GLSLF("c = texture(tex, base + pt * vec2(%d));\n", n);
        }
        GLSLF("color += vec4(weights[%d]) * c;\n", n);
        if (use_ar && (n == N/2-1 || n == N/2)) {
            GLSL(lo = min(lo, c);)
            GLSL(hi = max(hi, c);)
        }
    }
    if (use_ar)
        GLSLF("color = mix(color, clamp(color, lo, hi), %f);\n",
              scaler->conf.antiring);
    GLSLF("}\n");
}

void pass_sample_polar(struct gl_shader_cache *sc, struct scaler *scaler)
{
    double radius = scaler->kernel->f.radius;
    int bound = (int)ceil(radius);
    bool use_ar = scaler->conf.antiring > 0;
    GLSL(vec4 color = vec4(0.0);)
    GLSLF("{\n");
    GLSL(vec2 fcoord = fract(pos * size - vec2(0.5));)
    GLSL(vec2 base = pos - fcoord * pt;)
    GLSL(vec4 c;)
    GLSLF("float w, d, wsum = 0.0;\n");
    if (use_ar) {
        GLSL(vec4 lo = vec4(1.0);)
        GLSL(vec4 hi = vec4(0.0);)
    }
    gl_sc_uniform_sampler(sc, "lut", scaler->gl_target,
                          TEXUNIT_SCALERS + scaler->index);
    GLSLF("// scaler samples\n");
    for (int y = 1-bound; y <= bound; y++) {
        for (int x = 1-bound; x <= bound; x++) {
            // Since we can't know the subpixel position in advance, assume a
            // worst case scenario
            int yy = y > 0 ? y-1 : y;
            int xx = x > 0 ? x-1 : x;
            double dmax = sqrt(xx*xx + yy*yy);
            // Skip samples definitely outside the radius
            if (dmax >= radius)
                continue;
            GLSLF("d = length(vec2(%d, %d) - fcoord)/%f;\n", x, y, radius);
            // Check for samples that might be skippable
            if (dmax >= radius - 1)
                GLSLF("if (d < 1.0) {\n");
            GLSL(w = texture1D(lut, d).r;)
            GLSL(wsum += w;)
            GLSLF("c = texture(tex, base + pt * vec2(%d, %d));\n", x, y);
            GLSL(color += vec4(w) * c;)
            if (use_ar && x >= 0 && y >= 0 && x <= 1 && y <= 1) {
                GLSL(lo = min(lo, c);)
                GLSL(hi = max(hi, c);)
            }
            if (dmax >= radius -1)
                GLSLF("}\n");
        }
    }
    GLSL(color = color / vec4(wsum);)
    if (use_ar)
        GLSLF("color = mix(color, clamp(color, lo, hi), %f);\n",
              scaler->conf.antiring);
    GLSLF("}\n");
}

static void bicubic_calcweights(struct gl_shader_cache *sc, const char *t, const char *s)
{
    // Explanation of how bicubic scaling with only 4 texel fetches is done:
    //   http://www.mate.tue.nl/mate/pdfs/10318.pdf
    //   'Efficient GPU-Based Texture Interpolation using Uniform B-Splines'
    // Explanation why this algorithm normally always blurs, even with unit
    // scaling:
    //   http://bigwww.epfl.ch/preprints/ruijters1001p.pdf
    //   'GPU Prefilter for Accurate Cubic B-spline Interpolation'
    GLSLF("vec4 %s = vec4(-0.5, 0.1666, 0.3333, -0.3333) * %s"
                " + vec4(1, 0, -0.5, 0.5);\n", t, s);
    GLSLF("%s = %s * %s + vec4(0, 0, -0.5, 0.5);\n", t, t, s);
    GLSLF("%s = %s * %s + vec4(-0.6666, 0, 0.8333, 0.1666);\n", t, t, s);
    GLSLF("%s.xy *= vec2(1, 1) / vec2(%s.z, %s.w);\n", t, t, t);
    GLSLF("%s.xy += vec2(1 + %s, 1 - %s);\n", t, s, s);
}

void pass_sample_bicubic_fast(struct gl_shader_cache *sc)
{
    GLSL(vec4 color;)
    GLSLF("{\n");
    GLSL(vec2 fcoord = fract(pos * size + vec2(0.5, 0.5));)
    bicubic_calcweights(sc, "parmx", "fcoord.x");
    bicubic_calcweights(sc, "parmy", "fcoord.y");
    GLSL(vec4 cdelta;)
    GLSL(cdelta.xz = parmx.RG * vec2(-pt.x, pt.x);)
    GLSL(cdelta.yw = parmy.RG * vec2(-pt.y, pt.y);)
    // first y-interpolation
    GLSL(vec4 ar = texture(tex, pos + cdelta.xy);)
    GLSL(vec4 ag = texture(tex, pos + cdelta.xw);)
    GLSL(vec4 ab = mix(ag, ar, parmy.b);)
    // second y-interpolation
    GLSL(vec4 br = texture(tex, pos + cdelta.zy);)
    GLSL(vec4 bg = texture(tex, pos + cdelta.zw);)
    GLSL(vec4 aa = mix(bg, br, parmy.b);)
    // x-interpolation
    GLSL(color = mix(aa, ab, parmx.b);)
    GLSLF("}\n");
}

void pass_sample_oversample(struct gl_shader_cache *sc, struct scaler *scaler,
                                   int w, int h)
{
    GLSL(vec4 color;)
    GLSLF("{\n");
    GLSL(vec2 pos = pos + vec2(0.5) * pt;) // round to nearest
    GLSL(vec2 fcoord = fract(pos * size - vec2(0.5));)
    // We only need to sample from the four corner pixels since we're using
    // nearest neighbour and can compute the exact transition point
    GLSL(vec2 baseNW = pos - fcoord * pt;)
    GLSL(vec2 baseNE = baseNW + vec2(pt.x, 0.0);)
    GLSL(vec2 baseSW = baseNW + vec2(0.0, pt.y);)
    GLSL(vec2 baseSE = baseNW + pt;)
    // Determine the mixing coefficient vector
    gl_sc_uniform_vec2(sc, "output_size", (float[2]){w, h});
    GLSL(vec2 coeff = vec2((baseSE - pos) * output_size);)
    GLSL(coeff = clamp(coeff, 0.0, 1.0);)
    float threshold = scaler->conf.kernel.params[0];
    if (threshold > 0) { // also rules out NAN
        GLSLF("coeff = mix(coeff, vec2(0.0), "
              "lessThanEqual(coeff, vec2(%f)));\n", threshold);
        GLSLF("coeff = mix(coeff, vec2(1.0), "
              "greaterThanEqual(coeff, vec2(%f)));\n", 1.0 - threshold);
    }
    // Compute the right blend of colors
    GLSL(vec4 left = mix(texture(tex, baseSW),
                         texture(tex, baseNW),
                         coeff.y);)
    GLSL(vec4 right = mix(texture(tex, baseSE),
                          texture(tex, baseNE),
                          coeff.y);)
    GLSL(color = mix(right, left, coeff.x);)
    GLSLF("}\n");
}

// Linearize (expand), given a TRC as input
void pass_linearize(struct gl_shader_cache *sc, enum mp_csp_trc trc)
{
    if (trc == MP_CSP_TRC_LINEAR)
        return;

    GLSL(color.rgb = clamp(color.rgb, 0.0, 1.0);)
    switch (trc) {
    case MP_CSP_TRC_SRGB:
        GLSL(color.rgb = mix(color.rgb / vec3(12.92),
                             pow((color.rgb + vec3(0.055))/vec3(1.055), vec3(2.4)),
                             lessThan(vec3(0.04045), color.rgb));)
        break;
    case MP_CSP_TRC_BT_1886:
        GLSL(color.rgb = pow(color.rgb, vec3(1.961));)
        break;
    case MP_CSP_TRC_GAMMA18:
        GLSL(color.rgb = pow(color.rgb, vec3(1.8));)
        break;
    case MP_CSP_TRC_GAMMA22:
        GLSL(color.rgb = pow(color.rgb, vec3(2.2));)
        break;
    case MP_CSP_TRC_GAMMA28:
        GLSL(color.rgb = pow(color.rgb, vec3(2.8));)
        break;
    case MP_CSP_TRC_PRO_PHOTO:
        GLSL(color.rgb = mix(color.rgb / vec3(16.0),
                             pow(color.rgb, vec3(1.8)),
                             lessThan(vec3(0.03125), color.rgb));)
        break;
    }
}

// Delinearize (compress), given a TRC as output
void pass_delinearize(struct gl_shader_cache *sc, enum mp_csp_trc trc)
{
    if (trc == MP_CSP_TRC_LINEAR)
        return;

    GLSL(color.rgb = clamp(color.rgb, 0.0, 1.0);)
    switch (trc) {
    case MP_CSP_TRC_SRGB:
        GLSL(color.rgb = mix(color.rgb * vec3(12.92),
                             vec3(1.055) * pow(color.rgb, vec3(1.0/2.4))
                                 - vec3(0.055),
                             lessThanEqual(vec3(0.0031308), color.rgb));)
        break;
    case MP_CSP_TRC_BT_1886:
        GLSL(color.rgb = pow(color.rgb, vec3(1.0/1.961));)
        break;
    case MP_CSP_TRC_GAMMA18:
        GLSL(color.rgb = pow(color.rgb, vec3(1.0/1.8));)
        break;
    case MP_CSP_TRC_GAMMA22:
        GLSL(color.rgb = pow(color.rgb, vec3(1.0/2.2));)
        break;
    case MP_CSP_TRC_GAMMA28:
        GLSL(color.rgb = pow(color.rgb, vec3(1.0/2.8));)
        break;
    case MP_CSP_TRC_PRO_PHOTO:
        GLSL(color.rgb = mix(color.rgb * vec3(16.0),
                             pow(color.rgb, vec3(1.0/1.8)),
                             lessThanEqual(vec3(0.001953), color.rgb));)
        break;
    }
}

// Wide usage friendly PRNG, shamelessly stolen from a GLSL tricks forum post.
// Obtain random numbers by calling rand(h), followed by h = permute(h) to
// update the state.
static void prng_init(struct gl_shader_cache *sc, AVLFG *lfg)
{
    GLSLH(float mod289(float x)  { return x - floor(x / 289.0) * 289.0; })
    GLSLH(float permute(float x) { return mod289((34.0*x + 1.0) * x); })
    GLSLH(float rand(float x)    { return fract(x / 41.0); })

    // Initialize the PRNG by hashing the position + a random uniform
    GLSL(vec3 _m = vec3(pos, random) + vec3(1.0);)
    GLSL(float h = permute(permute(permute(_m.x)+_m.y)+_m.z);)
    gl_sc_uniform_f(sc, "random", (double)av_lfg_get(lfg) / UINT32_MAX);
}

struct deband_opts {
    int enabled;
    int iterations;
    float threshold;
    float range;
    float grain;
};

const struct deband_opts deband_opts_def = {
    .iterations = 4,
    .threshold = 64.0,
    .range = 8.0,
    .grain = 48.0,
};

#define OPT_BASE_STRUCT struct deband_opts
const struct m_sub_options deband_conf = {
    .opts = (const m_option_t[]) {
        OPT_INTRANGE("iterations", iterations, 0, 1, 16),
        OPT_FLOATRANGE("threshold", threshold, 0, 0.0, 4096.0),
        OPT_FLOATRANGE("range", range, 0, 1.0, 64.0),
        OPT_FLOATRANGE("grain", grain, 0, 0.0, 4096.0),
        {0}
    },
    .size = sizeof(struct deband_opts),
    .defaults = &deband_opts_def,
};

// Stochastically sample a debanded result from a given texture
void pass_sample_deband(struct gl_shader_cache *sc, struct deband_opts *opts,
                        int tex_num, GLenum tex_target, float tex_mul,
                        float img_w, float img_h, AVLFG *lfg)
{
    // Set up common variables and initialize the PRNG
    GLSLF("// debanding (tex %d)\n", tex_num);
    sampler_prelude(sc, tex_num);
    prng_init(sc, lfg);

    // Helper: Compute a stochastic approximation of the avg color around a
    // pixel
    GLSLHF("vec4 average(%s tex, vec2 pos, float range, inout float h) {",
           mp_sampler_type(tex_target));
        // Compute a random rangle and distance
        GLSLH(float dist = rand(h) * range;     h = permute(h);)
        GLSLH(float dir  = rand(h) * 6.2831853; h = permute(h);)

        GLSLHF("vec2 pt = dist / vec2(%f, %f);\n", img_w, img_h);
        GLSLH(vec2 o = vec2(cos(dir), sin(dir));)

        // Sample at quarter-turn intervals around the source pixel
        GLSLH(vec4 ref[4];)
        GLSLH(ref[0] = texture(tex, pos + pt * vec2( o.x,  o.y));)
        GLSLH(ref[1] = texture(tex, pos + pt * vec2(-o.y,  o.x));)
        GLSLH(ref[2] = texture(tex, pos + pt * vec2(-o.x, -o.y));)
        GLSLH(ref[3] = texture(tex, pos + pt * vec2( o.y, -o.x));)

        // Return the (normalized) average
        GLSLHF("return %f * (ref[0] + ref[1] + ref[2] + ref[3])/4.0;\n", tex_mul);
    GLSLH(})

    // Sample the source pixel
    GLSLF("vec4 color = %f * texture(tex, pos);\n", tex_mul);
    GLSLF("vec4 avg, diff;\n");
    for (int i = 1; i <= opts->iterations; i++) {
        // Sample the average pixel and use it instead of the original if
        // the difference is below the given threshold
        GLSLF("avg = average(tex, pos, %f, h);\n", i * opts->range);
        GLSL(diff = abs(color - avg);)
        GLSLF("color = mix(avg, color, greaterThan(diff, vec4(%f)));\n",
              opts->threshold / (i * 16384.0));
    }

    // Add some random noise to smooth out residual differences
    GLSL(vec3 noise;)
    GLSL(noise.x = rand(h); h = permute(h);)
    GLSL(noise.y = rand(h); h = permute(h);)
    GLSL(noise.z = rand(h); h = permute(h);)
    GLSLF("color.xyz += %f * (noise - vec3(0.5));\n", opts->grain/8192.0);
}

void pass_sample_unsharp(struct gl_shader_cache *sc, float param)
{
    GLSLF("// unsharp\n");
    sampler_prelude(sc, 0);

    GLSL(vec4 color;)
    GLSLF("{\n");
    GLSL(vec2 st1 = pt * 1.2;)
    GLSL(vec4 p = texture(tex, pos);)
    GLSL(vec4 sum1 = texture(tex, pos + st1 * vec2(+1, +1))
                   + texture(tex, pos + st1 * vec2(+1, -1))
                   + texture(tex, pos + st1 * vec2(-1, +1))
                   + texture(tex, pos + st1 * vec2(-1, -1));)
    GLSL(vec2 st2 = pt * 1.5;)
    GLSL(vec4 sum2 = texture(tex, pos + st2 * vec2(+1,  0))
                   + texture(tex, pos + st2 * vec2( 0, +1))
                   + texture(tex, pos + st2 * vec2(-1,  0))
                   + texture(tex, pos + st2 * vec2( 0, -1));)
    GLSL(vec4 t = p * 0.859375 + sum2 * -0.1171875 + sum1 * -0.09765625;)
    GLSLF("color = p + t * %f;\n", param);
    GLSLF("}\n");
}
