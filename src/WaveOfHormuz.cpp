#include "plugin.hpp"
#include <cmath>

// ---------------------------------------------------------------------------
// Windows CRT heap bridge
//
// libRack.dll is compiled against msvcrt.dll (MSVCRT private heap).
// Modern MSYS2 mingw64 defaults to UCRT (ucrtbase.dll, a separate heap).
// When Rack calls delete on objects our plugin allocated with new, it passes
// a UCRT pointer to MSVCRT's free() — RtlFreeHeap crash (signal 11).
//
// Fix: override operator new/delete to load msvcrt.dll at runtime and call
// its malloc/free directly, so every plugin allocation lives on the same
// heap that Rack's delete expects.
//
// Remove this block if targeting a Rack build that uses UCRT, or on macOS/Linux.
// ---------------------------------------------------------------------------
#ifdef ARCH_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <new>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
static void* msvcrt_malloc(size_t n) {
    static HMODULE dll = LoadLibraryA("msvcrt.dll");
    static auto*   fn  = reinterpret_cast<void*(*)(size_t)>(
                             GetProcAddress(dll, "malloc"));
    return fn(n);
}
static void msvcrt_free(void* p) {
    static HMODULE dll = LoadLibraryA("msvcrt.dll");
    static auto*   fn  = reinterpret_cast<void(*)(void*)>(
                             GetProcAddress(dll, "free"));
    fn(p);
}
#pragma GCC diagnostic pop

void* operator new  (size_t n)                  { void* p = msvcrt_malloc(n); if (!p) throw std::bad_alloc(); return p; }
void* operator new[](size_t n)                  { void* p = msvcrt_malloc(n); if (!p) throw std::bad_alloc(); return p; }
void  operator delete  (void* p) noexcept       { msvcrt_free(p); }
void  operator delete[](void* p) noexcept       { msvcrt_free(p); }
void  operator delete  (void* p, size_t) noexcept { msvcrt_free(p); }
void  operator delete[](void* p, size_t) noexcept { msvcrt_free(p); }
#endif

// ---------------------------------------------------------------------------
// The Wave of Hormuz — strait-shaping dual-closure oscillator  (14 HP)
//
// The base waveform encodes the 2023–2026 Strait of Hormuz conflict as a
// square wave:  +1 = OPEN (free transit),  −1 = CLOSED (blockade/seizure).
//
// Default timeline — 947 days from Oct 7 2023 to May 11 2026 (present):
//   0.0000 – 0.1996  (189 days)  OPEN
//   0.1996 – 0.2228  ( 22 days)  CLOSED — Iran seizes MV MSC Aries [C1]
//                                 (Apr 13 – May 5 2024)
//   0.2228 – 0.9261  (666 days)  OPEN
//   0.9261 – 1.0000  ( 70 days)  CLOSED — renewed blockade [C2]
//                                 (Mar 2 2026 – present)
//
// All knob defaults reproduce this timeline exactly.  Adjusting them
// morphs the historical waveform into new rhythmic or timbral territory.
//
// Controls:
//   KNOT SPEED       pitch (V/OCT + SWELL FM)
//   OPENING CEREMONY C1 closure phase start    (default: Apr 13 2024)
//   STRAIT JACKET    C1 closure width           (default: 22 days)
//   SANCTIONS        C2 closure phase start    (default: Mar 2 2026)
//   EMBARGO          C2 closure width           (default: 70 days / ongoing)
//   OIL SLICK        one-pole LP smooths square edges
//   CHOKE POINT      tanh soft-clip with normalised gain
//   PERSIAN TILT     shape inside closures: −1=ramp, 0=flat, +1=triangle
//   TANKER           output level (1 = ±5 V peak)
//   GULF/DRY         crossfade: 0=plain 50% square, 1=dual-closure wave
//
// Inputs : V/OCT, TIDE (hard sync), SWELL (FM),
//          OPEN (C1 start CV), LOCK (C2 start CV), TILT (Persian Tilt CV)
//          TILT CV is shared — it shapes both C1 and C2 simultaneously.
// Outputs: PASSAGE (audio ±5 V),  EOC (end-of-cycle trigger 10 V, 1 ms),
//          C1 (closure-1 gate 10 V),  C2 (closure-2 gate 10 V)
//
// Window overlap: C1 is checked first; if C1 and C2 overlap, C1 wins.
// ---------------------------------------------------------------------------

static constexpr float TWO_PI = 6.28318530717958647692f;

// Total timeline: 947 days (Oct 7 2023 – May 11 2026)
static constexpr float HORMUZ_C1_START = 189.f / 947.f; // 0.1996  Apr 13 2024
static constexpr float HORMUZ_C1_WIDTH =  22.f / 947.f; // 0.0232  22-day seizure (MSC Aries)
static constexpr float HORMUZ_C2_START = 877.f / 947.f; // 0.9261  Mar 2 2026
static constexpr float HORMUZ_C2_WIDTH =  70.f / 947.f; // 0.0739  ongoing (Mar 2 – May 11 2026)

// ---------------------------------------------------------------------------
// DSP helpers
// ---------------------------------------------------------------------------

// Returns true when ph falls inside [start, start+width) (wrapping).
// Sets local to normalised position within the window [0,1].
static bool phaseInWindow(float ph, float start, float width, float& local) {
    float end = start + width;
    if (end <= 1.f) {
        if (ph >= start && ph < end) {
            local = (ph - start) / width;
            return true;
        }
    } else {
        float wend = end - 1.f;
        if (ph >= start || ph < wend) {
            float rel = (ph >= start) ? (ph - start) : (1.f - start + ph);
            local = clamp(rel / width, 0.f, 1.f);
            return true;
        }
    }
    return false;
}

// Shapes the −1 closure value according to PERSIAN TILT.
static float closureShape(float lp, float tilt) {
    if (tilt >= 0.f) {
        float tri = 1.f - 2.f * std::abs(lp - 0.5f);
        return crossfade(-1.f, tri * 2.f - 1.f, tilt);
    }
    return crossfade(-1.f, lp * 2.f - 1.f, -tilt);
}

// ---------------------------------------------------------------------------
// Module
// ---------------------------------------------------------------------------

struct WaveOfHormuz : Module {

    enum ParamId {
        KNOT_SPEED_PARAM,
        OPENING_CEREMONY_PARAM,
        STRAIT_JACKET_PARAM,
        SANCTIONS_PARAM,
        EMBARGO_PARAM,
        OIL_SLICK_PARAM,
        CHOKE_POINT_PARAM,
        PERSIAN_TILT_PARAM,
        TANKER_PARAM,
        GULF_DRY_PARAM,
        PARAMS_LEN
    };

    enum InputId {
        VOCT_INPUT,
        TIDE_INPUT,
        SWELL_INPUT,
        OPENING_CV_INPUT,
        LOCK_CV_INPUT,
        TILT_CV_INPUT,
        INPUTS_LEN
    };

    enum OutputId {
        EOC_OUTPUT,
        C1_GATE_OUTPUT,
        C2_GATE_OUTPUT,
        PASSAGE_OUTPUT,
        OUTPUTS_LEN
    };

    enum LightId {
        PASSAGE_LIGHT,
        C1_GATE_LIGHT,
        C2_GATE_LIGHT,
        LIGHTS_LEN
    };

    float phase   = 0.f;
    float lpState = 0.f;

    dsp::PulseGenerator eocPulse;
    dsp::SchmittTrigger tideTrigger;

    WaveOfHormuz() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

        configParam(KNOT_SPEED_PARAM, -4.f, 4.f, 0.f,
                    "Knot Speed", " Hz", 2.f, dsp::FREQ_C4);
        configParam(OPENING_CEREMONY_PARAM, 0.f, 1.f, HORMUZ_C1_START,
                    "Opening Ceremony (C1 start)", "%", 0.f, 100.f);
        configParam(STRAIT_JACKET_PARAM, 0.f, 1.f, HORMUZ_C1_WIDTH,
                    "Strait Jacket (C1 width)", "%", 0.f, 100.f);
        configParam(SANCTIONS_PARAM, 0.f, 1.f, HORMUZ_C2_START,
                    "Sanctions (C2 start)", "%", 0.f, 100.f);
        configParam(EMBARGO_PARAM, 0.f, 1.f, HORMUZ_C2_WIDTH,
                    "Embargo (C2 width)", "%", 0.f, 100.f);
        configParam(OIL_SLICK_PARAM,    0.f,  1.f, 0.f, "Oil Slick (LP filter)");
        configParam(CHOKE_POINT_PARAM,  0.f,  1.f, 0.f, "Choke Point (tanh drive)");
        configParam(PERSIAN_TILT_PARAM, -1.f, 1.f, 0.f, "Persian Tilt (closure shape)");
        configParam(TANKER_PARAM,       0.f,  1.f, 1.f, "Tanker (output level)");
        configParam(GULF_DRY_PARAM,     0.f,  1.f, 1.f, "Gulf/Dry (wet-dry mix)");

        configInput(VOCT_INPUT,       "V/Oct");
        configInput(TIDE_INPUT,       "Tide (hard sync)");
        configInput(SWELL_INPUT,      "Swell (FM)");
        configInput(OPENING_CV_INPUT, "Opening Ceremony CV (C1 start)");
        configInput(LOCK_CV_INPUT,    "Lock CV (C2 start)");
        configInput(TILT_CV_INPUT,    "Tilt CV (Persian Tilt)");

        configOutput(EOC_OUTPUT,     "End of Crossing (trigger)");
        configOutput(C1_GATE_OUTPUT, "Closure 1 gate");
        configOutput(C2_GATE_OUTPUT, "Closure 2 gate");
        configOutput(PASSAGE_OUTPUT, "Passage (audio)");

        configLight(PASSAGE_LIGHT,  "Passage activity");
        configLight(C1_GATE_LIGHT,  "Closure 1 active");
        configLight(C2_GATE_LIGHT,  "Closure 2 active");
    }

    void process(const ProcessArgs& args) override {

        // --- Pitch ---
        float pitch = params[KNOT_SPEED_PARAM].getValue();
        if (inputs[VOCT_INPUT].isConnected())
            pitch += inputs[VOCT_INPUT].getVoltage();
        if (inputs[SWELL_INPUT].isConnected())
            pitch += inputs[SWELL_INPUT].getVoltage() * 0.25f;

        float freq = dsp::FREQ_C4 * std::pow(2.f, pitch);
        freq = clamp(freq, 0.f, args.sampleRate * 0.49f);

        phase += freq * args.sampleTime;
        bool eoc = false;
        if (phase >= 1.f) {
            phase -= std::floor(phase);
            eoc = true;
        }

        // --- TIDE: hard sync ---
        if (inputs[TIDE_INPUT].isConnected()) {
            if (tideTrigger.process(inputs[TIDE_INPUT].getVoltage(), 0.1f, 2.f)) {
                phase = 0.f;
                eoc   = true;
            }
        }

        // --- C1 closure (OPEN CV: 0.1× per volt) ---
        float c1Start = params[OPENING_CEREMONY_PARAM].getValue();
        if (inputs[OPENING_CV_INPUT].isConnected())
            c1Start += inputs[OPENING_CV_INPUT].getVoltage() * 0.1f;
        c1Start = clamp(c1Start, 0.f, 1.f);
        float c1Width = params[STRAIT_JACKET_PARAM].getValue();

        // --- C2 closure (LOCK CV: 0.1× per volt) ---
        float c2Start = params[SANCTIONS_PARAM].getValue();
        if (inputs[LOCK_CV_INPUT].isConnected())
            c2Start += inputs[LOCK_CV_INPUT].getVoltage() * 0.1f;
        c2Start = clamp(c2Start, 0.f, 1.f);
        float c2Width = params[EMBARGO_PARAM].getValue();

        // --- Persian Tilt (TILT CV: 0.2× per volt) ---
        float tilt = params[PERSIAN_TILT_PARAM].getValue();
        if (inputs[TILT_CV_INPUT].isConnected())
            tilt = clamp(tilt + inputs[TILT_CV_INPUT].getVoltage() * 0.2f, -1.f, 1.f);

        // --- Dual-closure passage ---
        bool  inC1 = false, inC2 = false;
        float wetWave = 1.f;
        float localPh;

        if (c1Width > 0.f && phaseInWindow(phase, c1Start, c1Width, localPh)) {
            inC1    = true;
            wetWave = closureShape(localPh, tilt);
        } else if (c2Width > 0.f && phaseInWindow(phase, c2Start, c2Width, localPh)) {
            inC2    = true;
            wetWave = closureShape(localPh, tilt);
        }

        float dryWave = (phase < 0.5f) ? 1.f : -1.f;
        float mixed   = crossfade(dryWave, wetWave, params[GULF_DRY_PARAM].getValue());

        // --- OIL SLICK: one-pole LP ---
        float slick = params[OIL_SLICK_PARAM].getValue();
        if (slick > 0.001f) {
            float cutoff = dsp::FREQ_C4 * std::pow(2.f, (1.f - slick) * 8.f - 4.f);
            cutoff = clamp(cutoff, 20.f, args.sampleRate * 0.49f);
            float alpha = 1.f - std::exp(-TWO_PI * cutoff * args.sampleTime);
            lpState += alpha * (mixed - lpState);
            mixed = lpState;
        } else {
            lpState = mixed;
        }

        // --- CHOKE POINT: tanh soft-clip ---
        float choke = params[CHOKE_POINT_PARAM].getValue();
        if (choke > 0.001f) {
            float drive = 1.f + choke * 9.f;
            mixed = std::tanh(mixed * drive) / std::tanh(drive);
        }

        float out = mixed * params[TANKER_PARAM].getValue() * 5.f;

        if (eoc) eocPulse.trigger(1e-3f);

        outputs[EOC_OUTPUT].setVoltage(eocPulse.process(args.sampleTime) ? 10.f : 0.f);
        outputs[C1_GATE_OUTPUT].setVoltage(inC1 ? 10.f : 0.f);
        outputs[C2_GATE_OUTPUT].setVoltage(inC2 ? 10.f : 0.f);
        outputs[PASSAGE_OUTPUT].setVoltage(out);

        lights[PASSAGE_LIGHT].setSmoothBrightness(std::abs(out) / 5.f, args.sampleTime);
        lights[C1_GATE_LIGHT].setBrightness(inC1 ? 1.f : 0.f);
        lights[C2_GATE_LIGHT].setBrightness(inC2 ? 1.f : 0.f);
    }
};

// ---------------------------------------------------------------------------
// PanelText — lightweight label widget.
//
// NanoSVG silently drops all <text> elements, so all visible labels must be
// drawn from C++.  This widget avoids ui::Label because that class has an
// explicit constructor in Rack.dll that initialises its std::string member;
// assigning to that field from our plugin code would mix two separate CRT
// heaps and corrupt the heap.  PanelText holds only a const char* to a
// string literal — no heap allocation, no cross-DLL ownership.
//
// Font handle 0 = DejaVuSans, which Rack always loads first.
// ---------------------------------------------------------------------------

struct PanelText : Widget {
    const char* text     = "";
    float       fontSize = 8.f;
    NVGcolor    color    = {};

    void draw(const DrawArgs& args) override {
        nvgFontFaceId(args.vg, 0);
        nvgFontSize(args.vg, fontSize);
        nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(args.vg, color);
        nvgText(args.vg, box.size.x * 0.5f, box.size.y * 0.5f, text, nullptr);
    }
};

// ---------------------------------------------------------------------------
// Widget  (14 HP = 71.12 mm)
//
// 4-column knob layout:
//   Columns x: 14.22, 28.44, 42.67, 56.89 mm   Centre: 35.56 mm
//
// 6 inputs  y=110  x: 7.62, 18.80, 29.98, 41.16, 52.34, 63.50
// 4 outputs y=121  x: 14.22, 28.44, 42.67, 56.89
// ---------------------------------------------------------------------------

struct WaveOfHormuzWidget : ModuleWidget {
    WaveOfHormuzWidget(WaveOfHormuz* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/WaveOfHormuz.svg")));

        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(
            Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ScrewSilver>(
            Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        // ── Knobs ────────────────────────────────────────────────────────
        // KNOT SPEED (large, centred)
        addParam(createParamCentered<RoundHugeBlackKnob>(
            mm2px(Vec(35.56f, 25.0f)), module, WaveOfHormuz::KNOT_SPEED_PARAM));

        // Closure row  y=47
        addParam(createParamCentered<RoundBlackKnob>(
            mm2px(Vec(14.22f, 47.0f)), module, WaveOfHormuz::OPENING_CEREMONY_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(
            mm2px(Vec(28.44f, 47.0f)), module, WaveOfHormuz::STRAIT_JACKET_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(
            mm2px(Vec(42.67f, 47.0f)), module, WaveOfHormuz::SANCTIONS_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(
            mm2px(Vec(56.89f, 47.0f)), module, WaveOfHormuz::EMBARGO_PARAM));

        // Effects row  y=71
        addParam(createParamCentered<RoundBlackKnob>(
            mm2px(Vec(14.22f, 71.0f)), module, WaveOfHormuz::OIL_SLICK_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(
            mm2px(Vec(28.44f, 71.0f)), module, WaveOfHormuz::CHOKE_POINT_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(
            mm2px(Vec(42.67f, 71.0f)), module, WaveOfHormuz::PERSIAN_TILT_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(
            mm2px(Vec(56.89f, 71.0f)), module, WaveOfHormuz::TANKER_PARAM));

        // GULF/DRY (small, centred)
        addParam(createParamCentered<RoundSmallBlackKnob>(
            mm2px(Vec(35.56f, 93.0f)), module, WaveOfHormuz::GULF_DRY_PARAM));

        // ── Inputs  (6, y=110) ───────────────────────────────────────────
        addInput(createInputCentered<PJ301MPort>(
            mm2px(Vec( 7.62f, 110.0f)), module, WaveOfHormuz::VOCT_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            mm2px(Vec(18.80f, 110.0f)), module, WaveOfHormuz::TIDE_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            mm2px(Vec(29.98f, 110.0f)), module, WaveOfHormuz::SWELL_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            mm2px(Vec(41.16f, 110.0f)), module, WaveOfHormuz::OPENING_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            mm2px(Vec(52.34f, 110.0f)), module, WaveOfHormuz::LOCK_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            mm2px(Vec(63.50f, 110.0f)), module, WaveOfHormuz::TILT_CV_INPUT));

        // ── Outputs  (4, y=121) ──────────────────────────────────────────
        addOutput(createOutputCentered<PJ301MPort>(
            mm2px(Vec(14.22f, 121.0f)), module, WaveOfHormuz::EOC_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            mm2px(Vec(28.44f, 121.0f)), module, WaveOfHormuz::C1_GATE_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            mm2px(Vec(42.67f, 121.0f)), module, WaveOfHormuz::C2_GATE_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            mm2px(Vec(56.89f, 121.0f)), module, WaveOfHormuz::PASSAGE_OUTPUT));

        // ── Activity lights ──────────────────────────────────────────────
        addChild(createLightCentered<SmallLight<GreenLight>>(
            mm2px(Vec(56.89f, 116.0f)), module, WaveOfHormuz::PASSAGE_LIGHT));
        addChild(createLightCentered<SmallLight<YellowLight>>(
            mm2px(Vec(28.44f, 116.0f)), module, WaveOfHormuz::C1_GATE_LIGHT));
        addChild(createLightCentered<SmallLight<RedLight>>(
            mm2px(Vec(42.67f, 116.0f)), module, WaveOfHormuz::C2_GATE_LIGHT));

        // ── Text labels ──────────────────────────────────────────────────
        // NanoSVG ignores <text> elements; all labels drawn via PanelText.
        NVGcolor cG = nvgRGB(0xe8, 0xc0, 0x34); // gold        (title)
        NVGcolor cP = nvgRGB(0x8e, 0xcf, 0xcf); // light teal  (primary knob names)
        NVGcolor cS = nvgRGB(0x5a, 0x8a, 0x9a); // muted teal  (jack names, sub-labels)
        NVGcolor cD = nvgRGB(0x3a, 0x68, 0x78); // dim teal    (range / secondary)

        // Helper: PanelText centred at (cx_mm, cy_mm), width wmm
        auto L = [&](float cx, float cy, const char* txt, float fs, NVGcolor c,
                     float wmm = 20.f) {
            float hPx = fs + 2.f;
            float wPx = mm2px(wmm);
            auto* w = new PanelText;
            w->box.pos  = mm2px(Vec(cx, cy)) - Vec(wPx * 0.5f, hPx * 0.5f);
            w->box.size = Vec(wPx, hPx);
            w->text     = txt;
            w->fontSize = fs;
            w->color    = c;
            addChild(w);
        };

        // Title
        L(35.56f,  6.2f, "THE WAVE OF HORMUZ",   8.f, cG, 66.f);

        // KNOT SPEED
        L(35.56f, 19.3f, "KNOT SPEED",           10.f, cP, 30.f);
        L(13.0f,  25.0f, "-4 OCT",                7.f, cD, 14.f);
        L(58.1f,  25.0f, "+4 OCT",                7.f, cD, 14.f);

        // Closure row header
        L(35.56f, 37.0f, "- CLOSURE WINDOWS -",   6.f, cD, 46.f);

        // Closure row knob labels  (y=47)
        L(14.22f, 39.2f, "OPENING",               9.f, cP);
        L(14.22f, 42.2f, "CEREMONY",              9.f, cP);
        L(14.22f, 54.5f, "c1 start",              7.f, cS);

        L(28.44f, 39.2f, "STRAIT",                9.f, cP);
        L(28.44f, 42.2f, "JACKET",                9.f, cP);
        L(28.44f, 54.5f, "c1 width",              7.f, cS);

        L(42.67f, 40.0f, "SANCTIONS",             9.f, cP);
        L(42.67f, 54.5f, "c2 start",              7.f, cS);

        L(56.89f, 40.0f, "EMBARGO",               9.f, cP);
        L(56.89f, 54.5f, "c2 width",              7.f, cS);

        // Effects row knob labels  (y=71)
        L(14.22f, 60.0f, "OIL SLICK",             9.f, cP);
        L(14.22f, 78.5f, "slew / LP",             7.f, cS);

        L(28.44f, 60.0f, "CHOKE",                 9.f, cP);
        L(28.44f, 63.0f, "POINT",                 9.f, cP);
        L(28.44f, 78.5f, "tanh drive",            7.f, cS);

        L(42.67f, 60.0f, "PERSIAN",               9.f, cP);
        L(42.67f, 63.0f, "TILT",                  9.f, cP);
        L(42.67f, 78.5f, "saw \xc2\xab tri",      7.f, cS);

        L(56.89f, 60.0f, "TANKER",                9.f, cP);
        L(56.89f, 78.5f, "amplitude",             7.f, cS);

        // GULF / DRY
        L(35.56f, 84.5f, "GULF / DRY",            9.f, cP, 34.f);
        L(20.5f,  93.0f, "DRY",                   7.f, cD, 12.f);
        L(50.6f,  93.0f, "WET",                   7.f, cD, 12.f);

        // Input jack labels  (y=110)
        L( 7.62f, 106.5f, "V/OCT",                8.f, cS, 13.f);
        L(18.80f, 106.5f, "TIDE",                  8.f, cS, 13.f);
        L(29.98f, 106.5f, "SWELL",                 8.f, cS, 13.f);
        L(41.16f, 106.5f, "OPEN",                  8.f, cS, 13.f);
        L(52.34f, 106.5f, "LOCK",                  8.f, cS, 13.f);
        L(63.50f, 106.5f, "TILT",                  8.f, cS, 13.f);

        // Input sub-labels
        L( 7.62f, 115.0f, "pitch",                 7.f, cD, 13.f);
        L(18.80f, 115.0f, "sync",                  7.f, cD, 13.f);
        L(29.98f, 115.0f, "fm",                    7.f, cD, 13.f);
        L(41.16f, 115.0f, "c1 cv",                 7.f, cD, 13.f);
        L(52.34f, 115.0f, "c2 cv",                 7.f, cD, 13.f);
        L(63.50f, 115.0f, "shp cv",                7.f, cD, 13.f);

        // Output secondary labels (above output jacks)
        L(14.22f, 117.5f, "trig",                  7.f, cD, 13.f);
        L(28.44f, 117.5f, "gate",                  7.f, cD, 13.f);
        L(42.67f, 117.5f, "gate",                  7.f, cD, 13.f);
        L(56.89f, 117.5f, "audio",                 7.f, cD, 13.f);

        // Output primary labels (below output jacks)
        L(14.22f, 125.5f, "EOC",                   8.f, cS, 13.f);
        L(28.44f, 125.5f, "C1",                    8.f, cS, 13.f);
        L(42.67f, 125.5f, "C2",                    8.f, cS, 13.f);
        L(56.89f, 125.5f, "PASSAGE",               8.f, cP, 18.f);
    }
};

Model* modelWaveOfHormuz =
    createModel<WaveOfHormuz, WaveOfHormuzWidget>("WaveOfHormuz");
