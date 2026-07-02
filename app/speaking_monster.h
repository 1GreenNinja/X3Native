#pragma once
// ===========================================================================
// SpeakingMonster — the HOST adapter that wires the dialog system's speaking
// state onto a SKINNED NPC (X3_WORLD_BLUEPRINT §3, requirement 4). Moved
// VERBATIM out of app/main.cpp (#28 monolith split) into x3::apphost so both
// the --demo-dialog dispatch (app/test_registry.cpp) and the --world hosts in
// main() share one definition. Header-only (the methods were inline in the
// class body in main.cpp); call sites use it unqualified via `using`.
//
// It implements x3::dialog::ISpeakingNpc and drives an x3::anim::Skinner
// READ-ONLY: on beginSpeaking it starts a "talk"/idle clip + records the
// subtitle; each frame while speaking it advances a head-bob (a small extra
// time scrub layered over the idle clip so the character reads as "talking");
// on endSpeaking it returns to rest. It owns its OWN Skinner bound to the NPC's
// Model so it never mutates the MonsterSystem (read-only use of anim). Lip-sync
// is not required — a talk pose / bob is the spec'd behaviour.
//
// Headless-safe: with a non-skinnable / absent model it still tracks the
// speaking lifecycle (begin/tick/end) so the demo + wiring are observable
// without a device.
// ===========================================================================

#include "dialog.h"   // x3::dialog::ISpeakingNpc / VoiceId
#include "anim.h"     // x3::anim::Skinner
#include "engine/asset/IModelLoader.h"  // x3::asset::Model
#include "engine/core/x3_log.h"

#include <string>
#include <string_view>
#include <vector>
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace x3::apphost {

class SpeakingMonster final : public x3::dialog::ISpeakingNpc {
public:
    // Bind to a loaded skinned Model (e.g. chief_martinez_anim.glb). The model must
    // outlive this adapter (the demo owns it). Picks a talk/idle clip by name.
    bool bind(const x3::asset::Model& model) {
        m_model = &model;
        m_skinnable = m_skinner.bind(model);
        if (m_skinnable) {
            // Prefer a "talk"/"idle" clip for the speaking pose; fall back to clip 0.
            m_talkClip = m_skinner.findClip({ "talk", "idle" });
            if (m_talkClip < 0 && m_skinner.clipCount() > 0) m_talkClip = 0;
        }
        return m_skinnable;
    }

    bool skinnable()  const { return m_skinnable; }
    bool speaking()   const { return m_speaking; }
    int  beginCount() const { return m_begins; }
    int  endCount()   const { return m_ends; }
    const std::string& subtitle() const { return m_subtitle; }
    // Max per-component change of the joint palette observed between consecutive
    // ticks while speaking — proves the talk-bob actually animated the skeleton.
    float maxPaletteDelta() const { return m_maxDelta; }

    void beginSpeaking(std::string_view line, x3::dialog::VoiceId voice,
                       float estDurationSec) override {
        (void)voice;
        ++m_begins;
        m_speaking = true;
        m_subtitle.assign(line);
        m_animTime = 0.0f;
        m_estDur   = estDurationSec > 0.0f ? estDurationSec : 1.0f;
        m_havePrev = false;
        // Show the subtitle on the console (the HUD path would call
        // IRenderDevice::drawHudText with this string each frame).
        x3::logInfo(std::string("[dialog] ") + std::string(line));
    }

    void tickSpeaking(float dt, float phase01) override {
        if (!m_speaking) return;
        // Talk bob: advance the clip time, modulated by a small sinusoid so the
        // head visibly bobs across the line (peaks mid-line, settles at the end).
        const float bob = 1.0f + 0.6f * std::sin(phase01 * 6.2831853f);
        m_animTime += dt * bob;
        if (m_skinnable && m_model && m_talkClip >= 0) {
            // READ-ONLY anim use: compute the palette at the talk-clip time WITHOUT a
            // device (the demo is headless). A real windowed host would instead call
            // m_skinner.apply(model, device, talkClip, time) to skin + draw.
            m_skinner.computePalette(*m_model, (uint32_t)m_talkClip, m_animTime, m_curPal);
            if (m_havePrev && m_prevPal.size() == m_curPal.size()) {
                float d = 0.0f;
                for (size_t i = 0; i < m_curPal.size(); ++i)
                    d = std::max(d, std::fabs(m_curPal[i] - m_prevPal[i]));
                m_maxDelta = std::max(m_maxDelta, d);
            }
            m_prevPal = m_curPal;
            m_havePrev = true;
        }
    }

    void endSpeaking() override {
        ++m_ends;
        m_speaking = false;
        m_subtitle.clear();
    }

private:
    const x3::asset::Model* m_model = nullptr;
    x3::anim::Skinner       m_skinner;
    bool   m_skinnable = false;
    int    m_talkClip  = -1;
    bool   m_speaking  = false;
    int    m_begins    = 0;
    int    m_ends      = 0;
    float  m_animTime  = 0.0f;
    float  m_estDur    = 1.0f;
    std::string m_subtitle;
    std::vector<float> m_curPal, m_prevPal;
    bool   m_havePrev  = false;
    float  m_maxDelta  = 0.0f;
};

} // namespace x3::apphost
