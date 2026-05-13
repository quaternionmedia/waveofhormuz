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

void* operator new  (size_t n)                    { void* p = msvcrt_malloc(n); if (!p) throw std::bad_alloc(); return p; }
void* operator new[](size_t n)                    { void* p = msvcrt_malloc(n); if (!p) throw std::bad_alloc(); return p; }
void  operator delete  (void* p) noexcept         { msvcrt_free(p); }
void  operator delete[](void* p) noexcept         { msvcrt_free(p); }
void  operator delete  (void* p, size_t) noexcept { msvcrt_free(p); }
void  operator delete[](void* p, size_t) noexcept { msvcrt_free(p); }
#endif

// ---------------------------------------------------------------------------
// The Wave of Hormuz — strait-shaping dual-closure oscillator  (16 HP)
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
// All knob defaults reproduce this timeline exactly at 947-day proportions.
//
// Controls (10 knobs, all RoundBlackKnob):
//   knot speed       pitch (V/OCT + SWELL FM)
//   opening ceremony C1 closure phase start    (default: Apr 13 2024)
//   strait jacket    C1 closure width           (default: 22 days)
//   sanctions        C2 closure phase start    (default: Mar 2 2026)
//   embargo          C2 closure width           (default: 70 days)
//   oil slick        one-pole LP smooths edges
//   choke point      tanh soft-clip drive
//   persian tilt     shape inside closures: −1=ramp, 0=flat, +1=triangle
//   tanker           output level (1 = ±5 V peak)
//   gulf/dry         crossfade: 0=plain 50% square, 1=dual-closure wave
//
// Inputs (12):
//   v/oct, tide (hard sync), swell (FM)
//   open (C1 start CV), jckt (C1 width CV), lock (C2 start CV), emgo (C2 width CV)
//   slck (oil slick CV), chok (choke point CV), tilt (persian tilt CV)
//   tnk  (tanker CV), dry cv (gulf/dry CV)
//   All parameter CVs: 0.1× per volt, clamped to valid range.
//   TILT CV is shared — applies to both C1 and C2 simultaneously.
//
// Outputs (4):
//   passage (audio ±5 V),  eoc (end-of-cycle trigger 10 V, 1 ms),
//   c1 (closure-1 gate 10 V),  c2 (closure-2 gate 10 V)
//
// Window overlap: C1 is checked first; C1 wins if windows share a phase region.
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
        // pitch / sync
        VOCT_INPUT,
        TIDE_INPUT,
        SWELL_INPUT,
        // closure CVs
        OPENING_CV_INPUT,
        JACKET_CV_INPUT,
        LOCK_CV_INPUT,
        EMBARGO_CV_INPUT,
        // effect CVs
        SLICK_CV_INPUT,
        CHOKE_CV_INPUT,
        TILT_CV_INPUT,
        TANKER_CV_INPUT,
        // mix CV
        DRY_CV_INPUT,
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
        configInput(OPENING_CV_INPUT, "Open CV (C1 start)");
        configInput(JACKET_CV_INPUT,  "Jacket CV (C1 width)");
        configInput(LOCK_CV_INPUT,    "Lock CV (C2 start)");
        configInput(EMBARGO_CV_INPUT, "Embargo CV (C2 width)");
        configInput(SLICK_CV_INPUT,   "Slick CV (LP filter)");
        configInput(CHOKE_CV_INPUT,   "Choke CV (tanh drive)");
        configInput(TILT_CV_INPUT,    "Tilt CV (closure shape)");
        configInput(TANKER_CV_INPUT,  "Tanker CV (output level)");
        configInput(DRY_CV_INPUT,     "Dry CV (wet-dry mix)");

        configOutput(EOC_OUTPUT,     "End of Crossing (trigger)");
        configOutput(C1_GATE_OUTPUT, "Closure 1 gate");
        configOutput(C2_GATE_OUTPUT, "Closure 2 gate");
        configOutput(PASSAGE_OUTPUT, "Passage (audio)");

        configLight(PASSAGE_LIGHT, "Passage activity");
        configLight(C1_GATE_LIGHT, "Closure 1 active");
        configLight(C2_GATE_LIGHT, "Closure 2 active");
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

        // --- C1 closure params ---
        float c1Start = params[OPENING_CEREMONY_PARAM].getValue();
        if (inputs[OPENING_CV_INPUT].isConnected())
            c1Start += inputs[OPENING_CV_INPUT].getVoltage() * 0.1f;
        c1Start = clamp(c1Start, 0.f, 1.f);

        float c1Width = params[STRAIT_JACKET_PARAM].getValue();
        if (inputs[JACKET_CV_INPUT].isConnected())
            c1Width = clamp(c1Width + inputs[JACKET_CV_INPUT].getVoltage() * 0.1f, 0.f, 1.f);

        // --- C2 closure params ---
        float c2Start = params[SANCTIONS_PARAM].getValue();
        if (inputs[LOCK_CV_INPUT].isConnected())
            c2Start += inputs[LOCK_CV_INPUT].getVoltage() * 0.1f;
        c2Start = clamp(c2Start, 0.f, 1.f);

        float c2Width = params[EMBARGO_PARAM].getValue();
        if (inputs[EMBARGO_CV_INPUT].isConnected())
            c2Width = clamp(c2Width + inputs[EMBARGO_CV_INPUT].getVoltage() * 0.1f, 0.f, 1.f);

        // --- Persian Tilt (shared across C1 and C2) ---
        float tilt = params[PERSIAN_TILT_PARAM].getValue();
        if (inputs[TILT_CV_INPUT].isConnected())
            tilt = clamp(tilt + inputs[TILT_CV_INPUT].getVoltage() * 0.2f, -1.f, 1.f);

        // --- Gulf/Dry mix ---
        float mix = params[GULF_DRY_PARAM].getValue();
        if (inputs[DRY_CV_INPUT].isConnected())
            mix = clamp(mix + inputs[DRY_CV_INPUT].getVoltage() * 0.1f, 0.f, 1.f);

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
        float mixed   = crossfade(dryWave, wetWave, mix);

        // --- Oil Slick: one-pole LP ---
        float slick = params[OIL_SLICK_PARAM].getValue();
        if (inputs[SLICK_CV_INPUT].isConnected())
            slick = clamp(slick + inputs[SLICK_CV_INPUT].getVoltage() * 0.1f, 0.f, 1.f);

        if (slick > 0.001f) {
            float cutoff = dsp::FREQ_C4 * std::pow(2.f, (1.f - slick) * 8.f - 4.f);
            cutoff = clamp(cutoff, 20.f, args.sampleRate * 0.49f);
            float alpha = 1.f - std::exp(-TWO_PI * cutoff * args.sampleTime);
            lpState += alpha * (mixed - lpState);
            mixed = lpState;
        } else {
            lpState = mixed;
        }

        // --- Choke Point: tanh soft-clip ---
        float choke = params[CHOKE_POINT_PARAM].getValue();
        if (inputs[CHOKE_CV_INPUT].isConnected())
            choke = clamp(choke + inputs[CHOKE_CV_INPUT].getVoltage() * 0.1f, 0.f, 1.f);

        if (choke > 0.001f) {
            float drive = 1.f + choke * 9.f;
            mixed = std::tanh(mixed * drive) / std::tanh(drive);
        }

        // --- Tanker: output level ---
        float tanker = params[TANKER_PARAM].getValue();
        if (inputs[TANKER_CV_INPUT].isConnected())
            tanker = clamp(tanker + inputs[TANKER_CV_INPUT].getVoltage() * 0.1f, 0.f, 1.f);

        float out = mixed * tanker * 5.f;
        if (!std::isfinite(out)) out = 0.f;

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
// Widget  (16 HP = 81.28 mm)
//
// 4 columns  x: 10.16  30.48  50.80  71.12
// knot speed x=20.32   gulf/dry x=60.96   (same row y=27, both RoundBlackKnob)
//
// Closure knobs  y=53   Closure CVs  y=66
// Effect  knobs  y=84   Effect  CVs  y=98
// I/O inputs     y=111  Outputs      y=121
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

        // ── Knobs ────────────────────────────────────────────────────────────
        // Top row: knot speed + gulf/dry — same size, same y
        addParam(createParamCentered<RoundBlackKnob>(
            mm2px(Vec(20.32f, 27.f)), module, WaveOfHormuz::KNOT_SPEED_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(
            mm2px(Vec(60.96f, 27.f)), module, WaveOfHormuz::GULF_DRY_PARAM));

        // Closure row  y=53
        addParam(createParamCentered<RoundBlackKnob>(
            mm2px(Vec(10.16f, 53.f)), module, WaveOfHormuz::OPENING_CEREMONY_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(
            mm2px(Vec(30.48f, 53.f)), module, WaveOfHormuz::STRAIT_JACKET_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(
            mm2px(Vec(50.80f, 53.f)), module, WaveOfHormuz::SANCTIONS_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(
            mm2px(Vec(71.12f, 53.f)), module, WaveOfHormuz::EMBARGO_PARAM));

        // Effect row  y=84
        addParam(createParamCentered<RoundBlackKnob>(
            mm2px(Vec(10.16f, 84.f)), module, WaveOfHormuz::OIL_SLICK_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(
            mm2px(Vec(30.48f, 84.f)), module, WaveOfHormuz::CHOKE_POINT_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(
            mm2px(Vec(50.80f, 84.f)), module, WaveOfHormuz::PERSIAN_TILT_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(
            mm2px(Vec(71.12f, 84.f)), module, WaveOfHormuz::TANKER_PARAM));

        // ── Closure CV inputs  y=66 ──────────────────────────────────────────
        addInput(createInputCentered<PJ301MPort>(
            mm2px(Vec(10.16f, 66.f)), module, WaveOfHormuz::OPENING_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            mm2px(Vec(30.48f, 66.f)), module, WaveOfHormuz::JACKET_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            mm2px(Vec(50.80f, 66.f)), module, WaveOfHormuz::LOCK_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            mm2px(Vec(71.12f, 66.f)), module, WaveOfHormuz::EMBARGO_CV_INPUT));

        // ── Effect CV inputs  y=98 ───────────────────────────────────────────
        addInput(createInputCentered<PJ301MPort>(
            mm2px(Vec(10.16f, 98.f)), module, WaveOfHormuz::SLICK_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            mm2px(Vec(30.48f, 98.f)), module, WaveOfHormuz::CHOKE_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            mm2px(Vec(50.80f, 98.f)), module, WaveOfHormuz::TILT_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            mm2px(Vec(71.12f, 98.f)), module, WaveOfHormuz::TANKER_CV_INPUT));

        // ── I/O inputs  y=111 ───────────────────────────────────────────────
        addInput(createInputCentered<PJ301MPort>(
            mm2px(Vec(10.16f, 111.f)), module, WaveOfHormuz::VOCT_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            mm2px(Vec(30.48f, 111.f)), module, WaveOfHormuz::TIDE_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            mm2px(Vec(50.80f, 111.f)), module, WaveOfHormuz::SWELL_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            mm2px(Vec(71.12f, 111.f)), module, WaveOfHormuz::DRY_CV_INPUT));

        // ── Outputs  y=121 ──────────────────────────────────────────────────
        addOutput(createOutputCentered<PJ301MPort>(
            mm2px(Vec(10.16f, 121.f)), module, WaveOfHormuz::EOC_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            mm2px(Vec(30.48f, 121.f)), module, WaveOfHormuz::C1_GATE_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            mm2px(Vec(50.80f, 121.f)), module, WaveOfHormuz::C2_GATE_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            mm2px(Vec(71.12f, 121.f)), module, WaveOfHormuz::PASSAGE_OUTPUT));

        // ── Activity lights  y=117 ──────────────────────────────────────────
        addChild(createLightCentered<SmallLight<GreenLight>>(
            mm2px(Vec(71.12f, 117.f)), module, WaveOfHormuz::PASSAGE_LIGHT));
        addChild(createLightCentered<SmallLight<YellowLight>>(
            mm2px(Vec(30.48f, 117.f)), module, WaveOfHormuz::C1_GATE_LIGHT));
        addChild(createLightCentered<SmallLight<RedLight>>(
            mm2px(Vec(50.80f, 117.f)), module, WaveOfHormuz::C2_GATE_LIGHT));

        // ── Text labels ──────────────────────────────────────────────────────
        // NanoSVG drops all <text> nodes; labels are drawn here via PanelText.
        NVGcolor cG = nvgRGB(0xe8, 0xc0, 0x34); // gold        (title)
        NVGcolor cP = nvgRGB(0x8e, 0xcf, 0xcf); // light teal  (knob names)
        NVGcolor cS = nvgRGB(0x5a, 0x8a, 0x9a); // muted teal  (jack names)
        NVGcolor cD = nvgRGB(0x3a, 0x68, 0x78); // dim teal    (range / cv labels)

        // wmm: edge cols (1,4) capped at 17 to stay inside rails
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
        L(40.64f,  6.2f, "the wave of hormuz",   8.f, cG, 76.f);

        // Top row: knot speed
        L(20.32f, 20.5f, "knot speed",            9.f, cP, 24.f);
        L( 7.0f,  33.0f, "-4 oct",                7.f, cD, 14.f);
        L(33.5f,  33.0f, "+4 oct",                7.f, cD, 14.f);

        // Top row: gulf/dry
        L(60.96f, 20.5f, "gulf / dry",            9.f, cP, 24.f);
        L(48.5f,  33.0f, "dry",                   7.f, cD, 10.f);
        L(73.5f,  33.0f, "wet",                   7.f, cD, 10.f);

        // Closure section header
        L(40.64f, 39.0f, "- closure windows -",   6.f, cD, 50.f);

        // Closure knob labels
        L(10.16f, 43.0f, "opening",               8.f, cP, 17.f);
        L(10.16f, 46.3f, "ceremony",              8.f, cP, 17.f);
        L(10.16f, 60.5f, "c1 start",              7.f, cS, 17.f);

        L(30.48f, 43.0f, "strait",                8.f, cP, 20.f);
        L(30.48f, 46.3f, "jacket",                8.f, cP, 20.f);
        L(30.48f, 60.5f, "c1 width",              7.f, cS, 20.f);

        L(50.80f, 44.7f, "sanctions",             8.f, cP, 20.f);
        L(50.80f, 60.5f, "c2 start",              7.f, cS, 20.f);

        L(71.12f, 44.7f, "embargo",               8.f, cP, 17.f);
        L(71.12f, 60.5f, "c2 width",              7.f, cS, 17.f);

        // Closure CV labels
        L(10.16f, 63.2f, "open",                  6.f, cD, 17.f);
        L(30.48f, 63.2f, "jckt",                  6.f, cD, 20.f);
        L(50.80f, 63.2f, "lock",                  6.f, cD, 20.f);
        L(71.12f, 63.2f, "emgo",                  6.f, cD, 17.f);

        // Effect knob labels
        L(10.16f, 74.0f, "oil",                   8.f, cP, 17.f);
        L(10.16f, 77.3f, "slick",                 8.f, cP, 17.f);
        L(10.16f, 91.5f, "slew / lp",             7.f, cS, 17.f);

        L(30.48f, 74.0f, "choke",                 8.f, cP, 20.f);
        L(30.48f, 77.3f, "point",                 8.f, cP, 20.f);
        L(30.48f, 91.5f, "tanh drive",            7.f, cS, 20.f);

        L(50.80f, 74.0f, "persian",               8.f, cP, 20.f);
        L(50.80f, 77.3f, "tilt",                  8.f, cP, 20.f);
        L(50.80f, 91.5f, "saw \xc2\xab tri",      7.f, cS, 20.f);

        L(71.12f, 75.7f, "tanker",                8.f, cP, 17.f);
        L(71.12f, 91.5f, "amplitude",             7.f, cS, 17.f);

        // Effect CV labels
        L(10.16f, 94.7f, "slck",                  6.f, cD, 17.f);
        L(30.48f, 94.7f, "chok",                  6.f, cD, 20.f);
        L(50.80f, 94.7f, "tilt",                  6.f, cD, 20.f);
        L(71.12f, 94.7f, "tnk",                   6.f, cD, 17.f);

        // I/O input labels
        L(10.16f, 105.5f, "v/oct",                8.f, cS, 17.f);
        L(30.48f, 105.5f, "tide",                 8.f, cS, 20.f);
        L(50.80f, 105.5f, "swell",                8.f, cS, 20.f);
        L(71.12f, 105.5f, "dry cv",               8.f, cS, 17.f);

        // Output type labels (above jacks)
        L(10.16f, 116.5f, "trig",                 7.f, cD, 17.f);
        L(30.48f, 116.5f, "gate",                 7.f, cD, 20.f);
        L(50.80f, 116.5f, "gate",                 7.f, cD, 20.f);
        L(71.12f, 116.5f, "audio",                7.f, cD, 17.f);

        // Output name labels (below jacks)
        L(10.16f, 125.5f, "eoc",                  8.f, cS, 17.f);
        L(30.48f, 125.5f, "c1",                   8.f, cS, 20.f);
        L(50.80f, 125.5f, "c2",                   8.f, cS, 20.f);
        L(71.12f, 125.5f, "passage",              8.f, cP, 17.f);
    }
};

Model* modelWaveOfHormuz =
    createModel<WaveOfHormuz, WaveOfHormuzWidget>("WaveOfHormuz");
