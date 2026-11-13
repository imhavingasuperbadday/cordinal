// Wind behind the track list (Preferences > View, "Wind behind the track list", off by default): soft
// ribbons that drift across the right of the list while music is heard, shared by both players.
//
// This file is only the motion. Each frame the player passes the heard audio's bass, mid and treble
// energy (Engine::WaveformBands) and the time since the last frame. A ribbon is the path its head has
// swept: the head steers along one current shared by the whole list, which bends and its eddies travel,
// and the path keeps a point every fixed spacing, so a ribbon's shape doesn't depend on the frame rate.
// A note attack sends a gust of ribbons along one line; loudness sets how often single ones appear, bass
// makes them wide and slow, and treble adds short quick wisps. Each one climbs or sinks a little as it
// goes and tapers to points at both ends, so it reads as silk rather than a line.
//
// The music shapes the wind while it plays, not only when a ribbon starts: bass deepens the current's
// broad arcs, treble adds fine wiggles, loud moments quicken it and widen every ribbon, and each beat
// sends a swell through it for a third of a second.
//
// A frame costs one step per ribbon and no trigonometry per point on screen: the shape is a path that
// was walked, not a sine evaluated for every row, and both front ends draw each ribbon as two filled
// shapes (a soft halo and a bright core) rather than a line per segment. Pause or silence starts nothing
// new and the ribbons already out drift off, so a player can stop its timer once Active() is false.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

struct WindStreak {
    static constexpr int kPoints = 22;  // the longest path kept, about 0.45 of the area's width

    float x[kPoints], y[kPoints];  // the swept path, tail first, as fractions of the area
    int count = 0;                 // points in use; two at birth, so a ribbon grows out of its origin
    int cap = 0;                   // points kept once grown: the ribbon's length
    float headX = 0, headY = 0;    // where the head is now
    float minY = 0, maxY = 0;      // the path's own top and bottom, for skipping rows it never crosses
    float angle = 0;               // radians from rightwards
    float wander = 0;              // how fast the angle is turning, radians a second
    float speed = 0;               // area widths a second
    float drift = 0;               // area heights a second, up or down
    float thick = 0;               // 0..1, how wide the ribbon is: bass and gust
    float weight = 0;              // 0..1, how strongly it shows at its brightest
    float age = 0, life = 0;       // seconds
};

class Wind {
public:
    static constexpr size_t kMaxStreaks = 24;
    static constexpr int kPoints = WindStreak::kPoints;
    // The core's most opaque point, as a share of the way from the row's background to the accent, and
    // the halo's share of that: low enough to sit under text, bright enough to read as light.
    static constexpr float kBrightest = 0.4f;
    static constexpr float kHalo = 0.42f;

    explicit Wind(uint32_t seed = 0x9E3779B9u) : rng_(seed ? seed : 1) { streaks_.reserve(kMaxStreaks); }

    // `energy` is one bucket of bass, mid and treble mean squares; `heard` is false while paused,
    // stopped or silent by choice, which lets the ribbons out finish but starts none.
    void Step(const float energy[3], bool heard, float seconds) {
        float dt = std::clamp(seconds, 0.0f, 0.1f);
        spawned_ = 0;
        float level[3] = {};
        for (int b = 0; b < 3; ++b) {
            // Each band against its own recent ceiling, so quiet and loud masters both move the wind;
            // the floor (about -60 dB) keeps hiss after a long silence from reading as full scale.
            float e = heard && energy[b] > 0 ? energy[b] : 0;
            ceiling_[b] = std::max({e, ceiling_[b] * std::exp(-dt / 8), 1e-6f});
            level[b] = std::sqrt(std::min(1.0f, e / ceiling_[b]));
        }
        float loud = (level[0] + level[1] + level[2]) / 3;
        // An attack is the raw energy jumping well over its last third of a second; the levels above
        // can't show one, since a steady sound of any loudness settles near its own ceiling.
        float total = heard ? energy[0] + energy[1] + energy[2] : 0;
        Breathe(energy, heard, dt);
        Flow(dt);
        cooldown_ -= dt;
        if (heard && loud > 0.2f && total > average_ * 2 + 1e-6f && cooldown_ <= 0) {
            // A gust: ribbons on one line with one heading, so it reads as one breath of wind.
            gustY_ = 0.1f + Random() * 0.8f;
            gustAngle_ = (Random() - 0.5f) * 0.18f;
            gustSpeed_ = 0.24f + 0.12f * loud + 0.08f * level[0];
            gustDrift_ = (Random() - 0.5f) * 0.04f;
            int count = 3 + (int)(Random() * 3);
            for (int i = 0; i < count; ++i) Spawn(level, loud, true);
            cooldown_ = 0.35f;
            pulse_ = 1;  // and the whole wind swells on it
        }
        average_ += (total - average_) * std::min(1.0f, dt * 3);
        spawn_ += heard ? dt * loud * (2.5f + 4.0f * drive_) : 0;
        for (; spawn_ >= 1; spawn_ -= 1) Spawn(level, loud, false);
        for (WindStreak& s : streaks_) Advance(s, dt);
        // A ribbon is gone only once its tail, not its head, has left the area: until then it is still
        // crossing the edge and melts into it (EdgeFade), rather than blinking out while much of it shows.
        std::erase_if(streaks_, [](const WindStreak& s) {
            return s.age >= s.life || s.x[0] > 1.0f || s.minY > 1.0f || s.maxY < 0.0f;
        });
    }

    bool Active() const { return !streaks_.empty(); }
    const std::vector<WindStreak>& Streaks() const { return streaks_; }
    // How many ribbons the last Step started: an attack's gust must not be read off the count, which
    // also drops the ones that ended in the same frame.
    int Spawned() const { return spawned_; }
    // 0.85..1.6, how wide the music holds every ribbon just now: it swells on a loud moment or a beat and
    // relaxes again. The players scale a ribbon's width and halo by it.
    float Swell() const { return 0.85f + 0.3f * drive_ + 0.42f * pulse_; }
    void Clear() {
        streaks_.clear();
        spawn_ = average_ = 0;
        spawned_ = 0;
        drive_ = pulse_ = 0;
        band_[0] = band_[1] = band_[2] = 0;
    }

    // 0..1, a ribbon's brightness across its life: it eases in as it grows and fades before it ends.
    static float Fade(const WindStreak& s) {
        return std::max(0.0f, std::min({1.0f, s.age / 0.35f, (s.life - s.age) / 0.9f}));
    }

    // 0..1, how strongly a point of a ribbon shows where it lies: a ribbon melts into the area's edges
    // instead of being cut off at them, over the last eighth of the width and a narrow band at the top
    // and bottom. Full brightness everywhere else, and zero past every edge.
    static float EdgeFade(float x, float y) {
        float across = std::clamp((1.0f - x) / 0.12f, 0.0f, 1.0f);
        float upDown = std::clamp(std::min(y, 1.0f - y) / 0.04f, 0.0f, 1.0f);
        float t = std::min(across, upDown);
        return t * t * (3.0f - 2.0f * t);  // smoothstep: no visible edge to the fade itself
    }

    // The ribbon's two edges in the pixels of an area `width` by `height`; `pad` widens it, which is how
    // the halo behind the core is made, and `scale` is the display's DPI factor. Both edges taper to
    // nothing at the ends, so the ribbon is a lens rather than a stroke, and the halo's extra width
    // melts away at an edge along with the brightness, so it never shows a cut. Returns the count.
    static int Ribbon(const WindStreak& s, float width, float height, float scale, float pad, float* lx, float* ly,
                      float* rx, float* ry) {
        if (s.count < 2) return 0;
        for (int i = 0; i < s.count; ++i) {
            int a = std::max(0, i - 1), b = std::min(s.count - 1, i + 1);  // the ends use their one segment
            float dx = (s.x[b] - s.x[a]) * width;
            float dy = (s.y[b] - s.y[a]) * height;
            float len = std::sqrt(dx * dx + dy * dy);
            float nx = len > 0.001f ? -dy / len : 0.0f, ny = len > 0.001f ? dx / len : -1.0f;
            float u = (float)i / (s.count - 1);
            float half = pad * EdgeFade(s.x[i], s.y[i]) + scale * (0.4f + 1.4f * s.thick) * 4 * u * (1 - u);
            float px = s.x[i] * width, py = s.y[i] * height;
            lx[i] = px + nx * half;
            ly[i] = py + ny * half;
            rx[i] = px - nx * half;
            ry[i] = py - ny * half;
        }
        return s.count;
    }

private:
    static constexpr float kSpacing = 0.022f;  // between path points, as a fraction of the area's width
    static constexpr int kFlow = 16;           // samples of the current across the area, a frame

    float Random() {  // xorshift32: cheap, and a fixed seed repeats in tests
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        return (rng_ >> 8) * (1.0f / 16777216.0f);
    }

    // One current for the whole list: three slow waves whose phases move at different speeds. Each
    // ribbon's head steers along it, so the wind bends as one body and its eddies travel across the list.
    // The music shapes it: bass deepens the broad arcs, treble adds fine wiggles, a loud moment quickens
    // it and a beat surges through it.
    void Flow(float dt) {
        flowT_ += dt * (0.25f + 0.65f * drive_ + 0.6f * pulse_);
        float broad = 0.12f + 0.13f * band_[0];
        float fine = 0.02f + 0.10f * band_[2];
        for (int i = 0; i < kFlow; ++i) {
            float x = (float)i / (kFlow - 1);
            flow_[i] = broad * std::sin(3.4f * x + flowT_) + broad * 0.8f * std::sin(7.2f * x - flowT_ * 0.7f + 1.7f) +
                       fine * std::sin(13.0f * x + flowT_ * 1.7f);
        }
    }
    float FlowAt(float x) const {
        float f = std::clamp(x, 0.0f, 1.0f) * (kFlow - 1);
        int i = std::min((int)f, kFlow - 2);
        return flow_[i] + (flow_[i + 1] - flow_[i]) * (f - i);
    }

    // The music's envelope, band by band. Each band is read against its own recent average rather than
    // its peak, so a quiet and a loud master both breathe and the wind follows the shape of the music
    // rather than its absolute level: `band_` feeds the current's bend and the ribbons' twitchiness,
    // `drive_` the overall quickness and width, and `pulse_` a beat's flash, gone within a third of a
    // second.
    void Breathe(const float energy[3], bool heard, float dt) {
        float sum = 0;
        for (int b = 0; b < 3; ++b) {
            float e = heard && energy[b] > 0 ? energy[b] : 0;
            // The resting point is the song's own level, not its peak, and it moves slowly enough that a
            // chorus stays above its verse; a pause leaves it alone, so a resumed track isn't a hit.
            if (heard) {
                if (room_[b] <= 0) room_[b] = e;  // the first sound heard sets it
                else room_[b] += (e - room_[b]) * std::min(1.0f, dt / 12);
            }
            // A steady band reads 0.55; twice the recent average reads just under 1, and a sudden loud
            // moment is clipped at 1.2.
            float live = heard ? std::clamp(std::sqrt(e / (room_[b] + 1e-9f)) * 0.55f, 0.0f, 1.2f) : 0.0f;
            band_[b] += (live - band_[b]) * std::min(1.0f, dt * (live > band_[b] ? 7.0f : 2.2f));
            sum += band_[b];
        }
        float aim = std::min(1.0f, sum * 0.37f);
        drive_ += (aim - drive_) * std::min(1.0f, dt * (aim > drive_ ? 6.0f : 1.6f));
        pulse_ *= std::exp(-dt / 0.28f);
    }

    void Advance(WindStreak& s, float dt) {
        s.age += dt;
        // The heading follows the current plus a turn of the ribbon's own that wanders and relaxes, so
        // the ribbons snake together without ever drawing the same curve twice. Treble makes their turns
        // twitchier, and a loud moment hurries them along.
        s.wander += (Random() - 0.5f) * (3.4f + 4.0f * band_[2]) * dt;
        s.wander *= std::exp(-dt / 0.6f);
        float rush = 0.8f + 0.35f * drive_ + 0.4f * pulse_;
        float steer = std::clamp(FlowAt(s.headX) + s.wander * 0.5f, -0.6f, 0.6f);
        s.angle += (steer - s.angle) * std::min(1.0f, dt * 8);
        s.headX += std::cos(s.angle) * s.speed * rush * dt;
        s.headY += std::sin(s.angle) * s.speed * rush * dt + s.drift * dt;
        // The last point is the live tip; a point is only committed once the head has travelled a fixed
        // spacing from the one before it, so a ribbon's shape doesn't depend on the frame rate.
        int ref = s.count - 2;  // the last committed point; the one after it is the live tip
        float dx = s.headX - s.x[ref], dy = s.headY - s.y[ref];
        if (dx * dx + dy * dy < kSpacing * kSpacing) {
            s.x[s.count - 1] = s.headX;
            s.y[s.count - 1] = s.headY;
        } else {
            if (s.count == s.cap) {  // the tail lets go by one point: the ribbon slides along its path
                for (int i = 1; i < s.count; ++i) {
                    s.x[i - 1] = s.x[i];
                    s.y[i - 1] = s.y[i];
                }
                --s.count;
            }
            s.x[s.count] = s.headX;
            s.y[s.count] = s.headY;
            ++s.count;
        }
        s.minY = s.maxY = s.y[0];
        for (int i = 1; i < s.count; ++i) {
            s.minY = std::min(s.minY, s.y[i]);
            s.maxY = std::max(s.maxY, s.y[i]);
        }
    }

    void Spawn(const float level[3], float loud, bool gust) {
        if (streaks_.size() >= kMaxStreaks) return;
        float bass = level[0], treble = level[2];
        bool wisp = Random() < treble / (bass + treble + 0.01f) * 0.8f;
        WindStreak s;
        s.headY = gust ? std::clamp(gustY_ + (Random() - 0.5f) * 0.2f, 0.04f, 0.96f) : 0.05f + Random() * 0.9f;
        s.headX = gust ? Random() * 0.45f : Random() * 0.55f;
        s.angle = std::clamp(FlowAt(s.headX) + (gust ? gustAngle_ : 0) + (Random() - 0.5f) * 0.2f, -0.6f, 0.6f);
        s.wander = (Random() - 0.5f) * 0.5f;
        s.speed = gust            ? gustSpeed_ * (0.92f + 0.16f * Random())
                  : wisp          ? 0.5f + 0.4f * Random()
                                  : 0.2f + 0.18f * Random() + 0.1f * bass;
        s.drift = gust ? gustDrift_ + (Random() - 0.5f) * 0.01f : (Random() - 0.5f) * (wisp ? 0.1f : 0.055f);
        s.thick = std::clamp(wisp ? 0.12f + 0.16f * Random() : 0.32f + 0.62f * bass + (gust ? 0.22f : 0), 0.0f, 1.0f);
        s.weight = 0.5f + 0.5f * (gust ? 1.0f : std::min(1.0f, loud + 0.2f));
        s.life = wisp ? 0.9f + 0.7f * Random() : 2.2f + 1.2f * Random();
        float length = wisp ? 0.16f + 0.1f * Random() : 0.3f + 0.22f * Random() + 0.15f * bass;
        s.cap = std::clamp((int)std::lround(length / kSpacing), 5, WindStreak::kPoints);
        s.count = 2;  // two points at the origin, so the ribbon grows out of it as it travels
        s.x[0] = s.x[1] = s.headX;
        s.y[0] = s.y[1] = s.headY;
        s.minY = s.maxY = s.headY;
        streaks_.push_back(s);
        ++spawned_;
    }

    std::vector<WindStreak> streaks_;
    uint32_t rng_;
    int spawned_ = 0;
    float band_[3] = {};                       // the music's smoothed bands, above 1 on a hit
    float room_[3] = {};                       // each band's recent average, the wind's resting point
    float drive_ = 0;                          // the music's overall envelope, 0..1
    float pulse_ = 0;                          // a beat's flash, 0..1
    float ceiling_[3] = {1e-6f, 1e-6f, 1e-6f};
    float average_ = 0, spawn_ = 0, cooldown_ = 0;
    float gustY_ = 0.5f, gustAngle_ = 0, gustSpeed_ = 0.3f, gustDrift_ = 0;
    float flow_[kFlow] = {};
    float flowT_ = 0;
};
