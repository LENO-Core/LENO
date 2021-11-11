
Debian
====================
This directory contains files used to package lenocored/lenocore-qt
for Debian-based Linux systems. If you compile lenocored/lenocore-qt yourself, there are some useful files here.

## lenocore: URI support ##


lenocore-qt.desktop  (Gnome / Open Desktop)
To install:

	sudo desktop-file-install lenocore-qt.desktop
	sudo update-desktop-database

If you build yourself, you will either need to modify the paths in
the .desktop file or copy or symlink your lenocoreqt binary to `/usr/bin`
and the `../../share/pixmaps/lenocore128.png` to `/usr/share/pixmaps`

lenocore-qt.protocol (KDE)

