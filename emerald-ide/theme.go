package main

import (
	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/theme"
)

func applyTheme(name string) {
	if fyne.CurrentApp() == nil {
		return
	}
	if name == "paper" {
		fyne.CurrentApp().Settings().SetTheme(theme.LightTheme())
	} else {
		fyne.CurrentApp().Settings().SetTheme(theme.DarkTheme())
	}
}
