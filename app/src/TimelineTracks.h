#pragma once

// THE TIMELINE'S TRACK STATE (the redesign's Part B, 2026-10-18): each of
// the tracks can be LOCKED (its content cannot be edited) and HIDDEN
// (the timeline paints it empty and ignores presses on it); the audio track
// can also be MUTED (silent in playback, left out of the MP4). The CAMERA
// track (Phase 1C) is the exception to "hidden": see cameraVisible. All the
// flags are the project's: saved in the manifest under "tracks", read back
// with every key defaulting to unlocked / visible / unmuted, so a project
// written before this loads exactly as it did. The toggles are NOT undo
// commands (the user's decision c): they change this state directly and
// mark the project unsaved.
//
// What a lock blocks is the page's business (StoryboardPage, MainWindow,
// AnimaticPage); what hidden means is the timeline's (AnimaticTimeline).
// This header only holds the state and its JSON form, so the three of them
// and ProjectIO agree on one definition.

#include <QJsonObject>
#include <QtGlobal>

enum class TimelineTrack { Scenes, Shots, Audio, Camera };

struct TimelineTracks
{
    bool scenesLocked = false;
    bool shotsLocked = false;
    bool audioLocked = false;
    bool scenesVisible = true;
    bool shotsVisible = true;
    bool audioVisible = true;
    int audioVolume = 100; // 0..100 (perceptual, Part D 2026-10-23); 0 = muted
    bool audioMuted() const { return audioVolume == 0; }
    // THE CAMERA TRACK (Phase 1C, 2026-10-28). Locked: no camera key can be
    // added, moved, changed or deleted, on the CAMERA row or with the
    // canvas's Camera tool. Hidden: the camera is off EVERYWHERE - the
    // preview, the SHOTS clips and every export show the whole panel - not
    // only on the timeline, as the other tracks' eyes are.
    bool cameraLocked = false;
    bool cameraVisible = true;

    bool locked(TimelineTrack t) const
    {
        switch (t) {
        case TimelineTrack::Scenes: return scenesLocked;
        case TimelineTrack::Shots: return shotsLocked;
        case TimelineTrack::Audio: return audioLocked;
        case TimelineTrack::Camera: return cameraLocked;
        }
        return false;
    }
    bool visible(TimelineTrack t) const
    {
        switch (t) {
        case TimelineTrack::Scenes: return scenesVisible;
        case TimelineTrack::Shots: return shotsVisible;
        case TimelineTrack::Audio: return audioVisible;
        case TimelineTrack::Camera: return cameraVisible;
        }
        return true;
    }
    void setLocked(TimelineTrack t, bool on)
    {
        switch (t) {
        case TimelineTrack::Scenes: scenesLocked = on; break;
        case TimelineTrack::Shots: shotsLocked = on; break;
        case TimelineTrack::Audio: audioLocked = on; break;
        case TimelineTrack::Camera: cameraLocked = on; break;
        }
    }
    void setVisible(TimelineTrack t, bool on)
    {
        switch (t) {
        case TimelineTrack::Scenes: scenesVisible = on; break;
        case TimelineTrack::Shots: shotsVisible = on; break;
        case TimelineTrack::Audio: audioVisible = on; break;
        case TimelineTrack::Camera: cameraVisible = on; break;
        }
    }

    bool operator==(const TimelineTracks &o) const
    {
        return scenesLocked == o.scenesLocked && shotsLocked == o.shotsLocked
            && audioLocked == o.audioLocked && scenesVisible == o.scenesVisible
            && shotsVisible == o.shotsVisible && audioVisible == o.audioVisible
            && audioVolume == o.audioVolume && cameraLocked == o.cameraLocked
            && cameraVisible == o.cameraVisible;
    }
    bool operator!=(const TimelineTracks &o) const { return !(*this == o); }

    // {"scenes":{"locked":false,"visible":true},
    //  "shots":{"locked":false,"visible":true},
    //  "audio":{"locked":false,"visible":true,"volume":100}}
    // and "camera":{"locked":..,"visible":..} ONLY when the camera track is
    // locked or hidden: a project that never touched it saves the JSON it
    // saved before the camera row existed.
    QJsonObject toJson() const
    {
        QJsonObject scenes, shots, audio;
        scenes[QStringLiteral("locked")] = scenesLocked;
        scenes[QStringLiteral("visible")] = scenesVisible;
        shots[QStringLiteral("locked")] = shotsLocked;
        shots[QStringLiteral("visible")] = shotsVisible;
        audio[QStringLiteral("locked")] = audioLocked;
        audio[QStringLiteral("visible")] = audioVisible;
        audio[QStringLiteral("volume")] = audioVolume;
        QJsonObject root;
        root[QStringLiteral("scenes")] = scenes;
        root[QStringLiteral("shots")] = shots;
        root[QStringLiteral("audio")] = audio;
        if (cameraLocked || !cameraVisible) {
            QJsonObject camera;
            camera[QStringLiteral("locked")] = cameraLocked;
            camera[QStringLiteral("visible")] = cameraVisible;
            root[QStringLiteral("camera")] = camera;
        }
        return root;
    }
    // Every missing key is the default: an old project (no "tracks" at
    // all) comes back unlocked, visible and unmuted.
    static TimelineTracks fromJson(const QJsonObject &root)
    {
        TimelineTracks t;
        const QJsonObject scenes = root.value(QStringLiteral("scenes")).toObject();
        const QJsonObject shots = root.value(QStringLiteral("shots")).toObject();
        const QJsonObject audio = root.value(QStringLiteral("audio")).toObject();
        t.scenesLocked = scenes.value(QStringLiteral("locked")).toBool(false);
        t.scenesVisible = scenes.value(QStringLiteral("visible")).toBool(true);
        t.shotsLocked = shots.value(QStringLiteral("locked")).toBool(false);
        t.shotsVisible = shots.value(QStringLiteral("visible")).toBool(true);
        t.audioLocked = audio.value(QStringLiteral("locked")).toBool(false);
        t.audioVisible = audio.value(QStringLiteral("visible")).toBool(true);
        const QJsonObject camera = root.value(QStringLiteral("camera")).toObject();
        t.cameraLocked = camera.value(QStringLiteral("locked")).toBool(false);
        t.cameraVisible = camera.value(QStringLiteral("visible")).toBool(true);
        // "volume" (Part D) wins; a Part B project saved with "muted": true
        // opens at 0 %, any other without a volume at 100 % (the user's rule).
        if (audio.contains(QStringLiteral("volume")))
            t.audioVolume = qBound(0, audio.value(QStringLiteral("volume")).toInt(100), 100);
        else
            t.audioVolume = audio.value(QStringLiteral("muted")).toBool(false) ? 0 : 100;
        return t;
    }
};
