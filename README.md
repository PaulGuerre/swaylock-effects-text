# swaylock-effects-text

Swaylock-effects-text is a fork of [swaylock](https://github.com/swaywm/swaylock) and [swaylock-effects](https://github.com/mortie/swaylock-effects) which adds the possibility to change the basic texts.

## Example Command

	swaylock \
		--text-clear Clear
		--text-caps-lock Maj
		--text-ver Check
		--text-wrong Error

## Installation

### Compiling from Source

Install dependencies:

* meson \*
* wayland
* wayland-protocols \*
* libxkbcommon
* cairo
* gdk-pixbuf2 \*\*
* pam (optional)
* [scdoc](https://git.sr.ht/~sircmpwn/scdoc) (optional: man pages) \*
* git \*
* openmp (if using a compiler other than GCC)

_\*Compile-time dep_
_\*\*Optional: required for background images other than PNG_

Run these commands:

	meson build
	ninja -C build
	sudo ninja -C build install

Swaylock will drop root permissions shortly after startup.
