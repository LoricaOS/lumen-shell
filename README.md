# lumen-shell

The LoricaOS desktop shell — the top bar (Aegis menu, the focused app's
File/Edit/… menu, volume popup, Wi-Fi/battery/CPU status, notifications).

Split out of [lumen](https://github.com/LoricaOS/lumen) (the compositor),
the same way [citadel-dock](https://github.com/LoricaOS/citadel-dock) was
split out earlier: this is an ordinary external client speaking lumen's
window protocol, not special-cased code inside the compositor. It creates a
top-anchored, full-width panel (`LUMEN_OP_CREATE_PANEL` / `LUMEN_PANEL_TOP`)
that's normally just the bar's height and grows downward via
`LUMEN_OP_RESIZE_SELF` to cover whichever dropdown or notification toast is
currently showing.

Two protocol pieces exist only for this client: `LUMEN_EV_MENU_STATE` (the
compositor pushes the focused window's published menu here, since the shell
doesn't own that window) and `LUMEN_OP_INVOKE_FOCUSED_MENU` (the shell asks
the compositor to invoke a chosen menu command on whichever window is
currently focused). Actions needing the compositor's own authority — the
About window, reboot/poweroff (POWER capability), locking the screen — go
back through the same `LUMEN_OP_INVOKE` mechanism citadel-dock already uses
to launch apps.

Depends on `lumen` (herald `depends=lumen`); ships as a mandatory part of the
desktop profile, not an optional app — without it there is no top bar.

Build: `make` (fetches the pinned glyph toolkit, builds `component.elf`,
packs the signed `.hpkg`). See `tools/pack.sh` for the per-component knobs.
