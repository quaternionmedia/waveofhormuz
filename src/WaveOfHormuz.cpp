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

// GCC warns about casting FARPROC (a function pointer) to a typed function
// pointer via reinterpret_cast.  On x64 Windows all pointer types are the
// same width, so the cast is safe.  Suppress the diagnostic locally.
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

void* operator new  (size_t n)           { void* p = msvcrt_malloc(n); if (!p) throw std::bad_alloc(); return p; }
void* operator new[](size_t n)           { void* p = msvcrt_malloc(n); if (!p) throw std::bad_alloc(); return p; }
void  operator delete  (void* p) noexcept { msvcrt_free(p); }
void  operator delete[](void* p) noexcept { msvcrt_free(p); }
void  operator delete  (void* p, size_t) noexcept { msvcrt_free(p); }
void  operator delete[](void* p, size_t) noexcept { msvcrt_free(p); }
#endif

// ---------------------------------------------------------------------------
// The Wave of Hormuz — strait-shaping square wave oscillator
//
// The base waveform encodes the 2023–2025 Strait of Hormuz conflict as a
// square wave:  +1 = OPEN (free transit),  −1 = CLOSED (blockade/seizure).
//
// Default timeline — 563 days from Oct 7 2023 to Apr 22 2025:
//   0.000 – 0.336  (189 days)  OPEN
//   0.336 – 0.375  ( 22 days)  CLOSED — Iran seizes MV MSC Aries
//                               (Apr 13 – May 5 2024)
//   0.375 – 1.000  (352 days)  OPEN
//
// Controls:
//   KNOT SPEED       pitch knob (V/OCT + SWELL FM inputs)
//   OPENING CEREMONY phase position where the closure begins
//   STRAIT JACKET    closure duration as a cycle fraction
//   OIL SLICK        one-pole LP smooths the hard square edges
//   CHOKE POINT      tanh soft-clip with normalised gain
//   PERSIAN TILT     shape inside the closure: 0=flat, +1=triangle, −1=ramp
//   TANKER           output level (1 = ±5 V peak)
//   GULF/DRY         crossfade: 0=plain 50% square, 1=conflict wave
//
// Inputs : V/OCT, TIDE (hard sync), SWELL (FM), OPEN (closure-start CV)
// Outputs: PASSAGE (audio ±5 V),  EOC (end-of-cycle trigger 10 V, 1 ms)
// ---------------------------------------------------------------------------

static constexpr float TWO_PI = 6.28318530717958647692f;

static constexpr float HORMUZ_CLOSURE_START = 189.f / 563.f; // 0.3356  Apr 13 2024
static constexpr float HORMUZ_CLOSURE_WIDTH =  22.f / 563.f; // 0.0391  22-day seizure

// ---------------------------------------------------------------------------
// Module
// ---------------------------------------------------------------------------

struct WaveOfHormuz : Module {

    enum ParamId {
        KNOT_SPEED_PARAM,
        OPENING_CEREMONY_PARAM,
        STRAIT_JACKET_PARAM,
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
        INPUTS_LEN
    };

    enum OutputId {
        PASSAGE_OUTPUT,
        EOC_OUTPUT,
        OUTPUTS_LEN
    };

    enum LightId {
        PASSAGE_LIGHT,
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
        configParam(OPENING_CEREMONY_PARAM, 0.f, 1.f, HORMUZ_CLOSURE_START,
                    "Opening Ceremony (closure start)", "%", 0.f, 100.f);
        configParam(STRAIT_JACKET_PARAM, 0.f, 1.f, HORMUZ_CLOSURE_WIDTH,
                    "Strait Jacket (closure width)", "%", 0.f, 100.f);
        configParam(OIL_SLICK_PARAM,    0.f,  1.f, 0.f, "Oil Slick (LP filter)");
        configParam(CHOKE_POINT_PARAM,  0.f,  1.f, 0.f, "Choke Point (tanh drive)");
        configParam(PERSIAN_TILT_PARAM, -1.f, 1.f, 0.f, "Persian Tilt (closure shape)");
        configParam(TANKER_PARAM,       0.f,  1.f, 1.f, "Tanker (output level)");
        configParam(GULF_DRY_PARAM,     0.f,  1.f, 1.f, "Gulf/Dry (wet-dry mix)");

        configInput(VOCT_INPUT,       "V/Oct");
        configInput(TIDE_INPUT,       "Tide (hard sync)");
        configInput(SWELL_INPUT,      "Swell (FM)");
        configInput(OPENING_CV_INPUT, "Opening Ceremony CV");

        configOutput(PASSAGE_OUTPUT, "Passage (audio)");
        configOutput(EOC_OUTPUT,     "End of Crossing (EOC trigger)");

        configLight(PASSAGE_LIGHT, "Passage activity");
    }

    // Returns +1 (OPEN) outside the closure window,
    //         −1 (CLOSED) inside it (flat or PERSIAN TILT-shaped).
    float computePassage(float ph, float closureStart, float closureWidth, float tilt) {
        if (closureWidth <= 0.f) return  1.f;
        if (closureWidth >= 1.f) return -1.f;

        float closureEnd = closureStart + closureWidth;
        bool  inClosure  = false;
        float localPhase = 0.f;

        if (closureEnd <= 1.f) {
            if (ph >= closureStart && ph < closureEnd) {
                localPhase = (ph - closureStart) / closureWidth;
                inClosure  = true;
            }
        } else {
            // Closure window wraps around the cycle boundary
            float wrappedEnd = closureEnd - 1.f;
            if (ph >= closureStart || ph < wrappedEnd) {
                float rel  = (ph >= closureStart) ? (ph - closureStart)
                                                  : (1.f - closureStart + ph);
                localPhase = rel / closureWidth;
                inClosure  = true;
            }
        }

        if (!inClosure) return 1.f;

        localPhase = clamp(localPhase, 0.f, 1.f);

        if (tilt >= 0.f) {
            // +tilt: triangle peak inside closure (partial reopening arc)
            float tri = 1.f - 2.f * std::abs(localPhase - 0.5f);
            return crossfade(-1.f, tri * 2.f - 1.f, tilt);
        } else {
            // −tilt: rising ramp inside closure (slow bleed-through)
            float saw = localPhase * 2.f - 1.f;
            return crossfade(-1.f, saw, -tilt);
        }
    }

    void process(const ProcessArgs& args) override {

        // Pitch (V/OCT + SWELL FM at ¼× sensitivity)
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

        // TIDE: hard sync — reset phase on rising edge
        if (inputs[TIDE_INPUT].isConnected()) {
            if (tideTrigger.process(inputs[TIDE_INPUT].getVoltage(), 0.1f, 2.f)) {
                phase = 0.f;
                eoc   = true;
            }
        }

        // Closure parameters (OPEN CV adds 0.1× per volt)
        float closureStart = params[OPENING_CEREMONY_PARAM].getValue();
        if (inputs[OPENING_CV_INPUT].isConnected())
            closureStart += inputs[OPENING_CV_INPUT].getVoltage() * 0.1f;
        closureStart = clamp(closureStart, 0.f, 1.f);

        float closureWidth = params[STRAIT_JACKET_PARAM].getValue();
        float tilt         = params[PERSIAN_TILT_PARAM].getValue();

        float dryWave = (phase < 0.5f) ? 1.f : -1.f;
        float wetWave = computePassage(phase, closureStart, closureWidth, tilt);
        float mixed   = crossfade(dryWave, wetWave, params[GULF_DRY_PARAM].getValue());

        // OIL SLICK: one-pole LP (exponential RC, cutoff mapped from knob)
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

        // CHOKE POINT: tanh soft-clip with unity-gain normalisation
        float choke = params[CHOKE_POINT_PARAM].getValue();
        if (choke > 0.001f) {
            float drive = 1.f + choke * 9.f;
            mixed = std::tanh(mixed * drive) / std::tanh(drive);
        }

        float out = mixed * params[TANKER_PARAM].getValue() * 5.f;

        if (eoc) eocPulse.trigger(1e-3f);

        outputs[PASSAGE_OUTPUT].setVoltage(out);
        outputs[EOC_OUTPUT].setVoltage(eocPulse.process(args.sampleTime) ? 10.f : 0.f);
        lights[PASSAGE_LIGHT].setSmoothBrightness(std::abs(out) / 5.f, args.sampleTime);
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
// Widget
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

        // Knobs
        addParam(createParamCentered<RoundHugeBlackKnob>(
            mm2px(Vec(25.4f, 24.0f)), module, WaveOfHormuz::KNOT_SPEED_PARAM));

        addParam(createParamCentered<RoundBlackKnob>(
            mm2px(Vec(12.7f, 48.0f)), module, WaveOfHormuz::OPENING_CEREMONY_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(
            mm2px(Vec(38.1f, 48.0f)), module, WaveOfHormuz::STRAIT_JACKET_PARAM));

        addParam(createParamCentered<RoundBlackKnob>(
            mm2px(Vec(12.7f, 68.0f)), module, WaveOfHormuz::OIL_SLICK_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(
            mm2px(Vec(38.1f, 68.0f)), module, WaveOfHormuz::CHOKE_POINT_PARAM));

        addParam(createParamCentered<RoundBlackKnob>(
            mm2px(Vec(12.7f, 88.0f)), module, WaveOfHormuz::PERSIAN_TILT_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(
            mm2px(Vec(38.1f, 88.0f)), module, WaveOfHormuz::TANKER_PARAM));

        addParam(createParamCentered<RoundSmallBlackKnob>(
            mm2px(Vec(25.4f, 101.0f)), module, WaveOfHormuz::GULF_DRY_PARAM));

        // Inputs
        addInput(createInputCentered<PJ301MPort>(
            mm2px(Vec( 7.62f, 111.0f)), module, WaveOfHormuz::VOCT_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            mm2px(Vec(17.78f, 111.0f)), module, WaveOfHormuz::TIDE_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            mm2px(Vec(27.94f, 111.0f)), module, WaveOfHormuz::SWELL_INPUT));
        addInput(createInputCentered<PJ301MPort>(
            mm2px(Vec(38.10f, 111.0f)), module, WaveOfHormuz::OPENING_CV_INPUT));

        // Outputs
        addOutput(createOutputCentered<PJ301MPort>(
            mm2px(Vec( 7.62f, 121.0f)), module, WaveOfHormuz::EOC_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(
            mm2px(Vec(43.18f, 121.0f)), module, WaveOfHormuz::PASSAGE_OUTPUT));

        addChild(createLightCentered<SmallLight<GreenLight>>(
            mm2px(Vec(43.18f, 115.5f)), module, WaveOfHormuz::PASSAGE_LIGHT));

        // ── Text labels ──────────────────────────────────────────────────
        // NanoSVG ignores <text> elements; all labels are drawn via PanelText.
        // Coordinates match the SVG design (mm). Colors from the panel palette.
        NVGcolor cG = nvgRGB(0xe8, 0xc0, 0x34); // gold        (title)
        NVGcolor cP = nvgRGB(0x8e, 0xcf, 0xcf); // light teal  (knob names)
        NVGcolor cS = nvgRGB(0x5a, 0x8a, 0x9a); // muted teal  (jack names, sub-labels)
        NVGcolor cD = nvgRGB(0x3a, 0x68, 0x78); // dim teal    (range / secondary text)

        // Helper: PanelText centred at (cx_mm, cy_mm), width wmm (default 28 mm)
        auto L = [&](float cx, float cy, const char* txt, float fs, NVGcolor c,
                     float wmm = 28.f) {
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
        L(25.4f,  6.4f, "THE WAVE OF HORMUZ",  8.f, cG, 46.f);

        // KNOT SPEED
        L(25.4f, 19.0f, "KNOT SPEED",          10.f, cP);
        L( 5.5f, 24.0f, "-4 OCT",               7.f, cD);
        L(45.3f, 24.0f, "+4 OCT",               7.f, cD);

        // OPENING CEREMONY
        L(12.7f, 37.0f, "OPENING",              9.f, cP);
        L(12.7f, 40.5f, "CEREMONY",             9.f, cP);
        L(12.7f, 55.0f, "phase offset",         7.f, cS);

        // STRAIT JACKET
        L(38.1f, 37.0f, "STRAIT",               9.f, cP);
        L(38.1f, 40.5f, "JACKET",               9.f, cP);
        L(38.1f, 55.0f, "duty cycle",           7.f, cS);

        // OIL SLICK
        L(12.7f, 60.0f, "OIL SLICK",            9.f, cP);
        L(12.7f, 75.0f, "slew / LP",            7.f, cS);

        // CHOKE POINT
        L(38.1f, 59.0f, "CHOKE",                9.f, cP);
        L(38.1f, 62.5f, "POINT",                9.f, cP);
        L(38.1f, 75.0f, "tanh drive",           7.f, cS);

        // PERSIAN TILT
        L(12.7f, 79.0f, "PERSIAN",              9.f, cP);
        L(12.7f, 82.5f, "TILT",                 9.f, cP);
        L(12.7f, 94.0f, "saw \xc2\xab tri",     7.f, cS);

        // TANKER
        L(38.1f, 80.5f, "TANKER",               9.f, cP);
        L(38.1f, 94.0f, "amplitude",            7.f, cS);

        // GULF / DRY
        L(25.4f, 96.5f,  "GULF / DRY",          9.f, cP);
        L(11.0f, 101.5f, "DRY",                 7.f, cD);
        L(39.8f, 101.5f, "WET",                 7.f, cD);

        // Input jacks
        L( 7.62f, 106.5f, "V/OCT",              8.f, cS);
        L(17.78f, 106.5f, "TIDE",               8.f, cS);
        L(27.94f, 106.5f, "SWELL",              8.f, cS);
        L(38.10f, 106.5f, "OPEN",               8.f, cS);
        L( 7.62f, 116.0f, "pitch",              7.f, cD);
        L(17.78f, 116.0f, "sync",               7.f, cD);
        L(27.94f, 116.0f, "fm",                 7.f, cD);
        L(38.10f, 116.0f, "cv",                 7.f, cD);

        // Output jacks
        L( 7.62f, 119.0f, "trig",               7.f, cD);
        L(43.18f, 119.0f, "audio",              7.f, cD);
        L( 7.62f, 125.5f, "EOC",                8.f, cS);
        L(43.18f, 125.5f, "PASSAGE",            8.f, cP);
    }
};

Model* modelWaveOfHormuz =
    createModel<WaveOfHormuz, WaveOfHormuzWidget>("WaveOfHormuz");
