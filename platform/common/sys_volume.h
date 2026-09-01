#ifndef BD_SYS_VOLUME_H
#define BD_SYS_VOLUME_H

// Portable system-volume overlay for Linux handhelds.
//
// Auto backend (default):
//   - Pulse/PipeWire: pass-through (mixer already applies volume; do not
//     software-scale, do not steal VOLUMEUP/DOWN).
//   - Known sysfs volume file (Anbernic openbor_volume and clones): software
//     gain from that file; intercept volume keys and write the file back.
//   - Otherwise: pass-through (ALSA/Pulse mixer, CFW daemon, etc.).
//
// Override via toml [audio] or env BD_SYS_VOLUME_PATH / BD_SYS_VOLUME_MAX /
// BD_SYS_VOLUME_BACKEND.

int bd_sys_volume_percent();          // 0–100
void bd_sys_volume_poll();            // re-read external source if any
void bd_sys_volume_adjust(int delta); // ±1 step
bool bd_sys_volume_owns_keys();       // intercept SDL VOLUMEUP/DOWN
const char* bd_sys_volume_backend_name();

#endif
