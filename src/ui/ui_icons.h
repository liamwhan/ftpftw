// Hand-drawn folder/file icons (UI_ namespace) - flat CPU pixel fill,
// packed into the shared font atlas once at startup (see
// fnt_atlas_pack_bitmap), no vendored icon assets.

#ifndef UI_ICONS_H
#define UI_ICONS_H

internal void ui_icons_init(void);
internal void ui_icon_uv(B32 is_dir, F32 *out_u0, F32 *out_v0, F32 *out_u1, F32 *out_v1);
internal void ui_icon_conn_uv(F32 *out_u0, F32 *out_v0, F32 *out_u1, F32 *out_v1);
internal void ui_icon_settings_uv(F32 *out_u0, F32 *out_v0, F32 *out_u1, F32 *out_v1);

#endif // UI_ICONS_H
