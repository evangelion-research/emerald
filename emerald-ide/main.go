package main

import (
	"fmt"
	"image/color"
	"os"
	"path/filepath"
	"strings"
	"time"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/app"
	"fyne.io/fyne/v2/canvas"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/dialog"
	"fyne.io/fyne/v2/driver/desktop"
	"fyne.io/fyne/v2/layout"
	"fyne.io/fyne/v2/widget"
)

const welcomeSource = `# Emerald - accepted-prefix proof session
# Cmd+Down advance | Cmd+Up retract | Cmd+Right check to cursor | Cmd+Enter check all

import math

type Shape = Circle | Square

def area(s: Shape) -> float pure {
    match s {
        circle -> { return 3.14159 * circle.r ** 2.0 },
        square -> { return square.side ** 2.0 }
    }
}

def contradiction(p: int) -> never pure {
    return fail("unreachable")
}

x: never = contradiction(1)
`

type IDE struct {
	app       fyne.App
	window    fyne.Window
	editor    *widget.Entry
	gutter    *widget.Label
	status    *widget.Label
	session   *Session
	panels    *PanelView
	filePath  string
	compiler  CompilerInfo
	themeMenu *widget.Select
}

func newIDE(a fyne.App, w fyne.Window) *IDE {
	ide := &IDE{app: a, window: w, session: NewSession(), editor: widget.NewMultiLineEntry()}
	ide.editor.Wrapping = fyne.TextWrapOff
	ide.editor.SetMinRowsVisible(20)
	ide.editor.TextStyle = fyne.TextStyle{Monospace: true}
	ide.editor.SetText(welcomeSource)
	ide.editor.OnChanged = func(text string) {
		ide.session.SetText(text, caretByte(ide.editor), false)
		ide.render()
	}
	ide.editor.OnCursorChanged = func() { ide.renderStatus() }

	ide.gutter = widget.NewLabel("")
	ide.gutter.TextStyle = fyne.TextStyle{Monospace: true}
	ide.gutter.Alignment = fyne.TextAlignTrailing
	ide.gutter.Importance = widget.LowImportance
	ide.status = widget.NewLabel("")
	ide.status.TextStyle = fyne.TextStyle{Monospace: true}
	ide.panels = NewPanelView(ide.session, func(line int) { ide.scrollToLine(line) })
	ide.themeMenu = widget.NewSelect([]string{"emerald", "tokyo", "mono", "paper"}, func(name string) {
		applyTheme(name)
	})
	ide.themeMenu.SetSelected("emerald")
	ide.compiler = ResolveCompiler()
	ide.session.SetCompiler(ide.compiler)
	ide.session.OnUpdate = func() { ide.render() }
	ide.session.SetText(welcomeSource, 0, true)
	ide.bindShortcuts()
	return ide
}

func (ide *IDE) content() fyne.CanvasObject {
	brand := canvas.NewText("emerald·ide", color.NRGBA{R: 61, G: 220, B: 151, A: 255})
	brand.TextStyle = fyne.TextStyle{Monospace: true, Bold: true}
	crumb := widget.NewLabel("untitled.rald")
	ideStatus := container.NewHBox(brand, crumb, layout.NewSpacer(), ide.themeMenu)

	editorPane := container.NewBorder(nil, nil, container.NewBorder(nil, nil, nil, ide.gutter), nil,
		container.NewScroll(ide.editor))
	workspace := container.NewHSplit(editorPane, ide.panels.Content())
	workspace.SetOffset(0.68)

	advance := widget.NewButton("▶", func() { ide.session.Advance() })
	retract := widget.NewButton("◀", func() { ide.session.Retract() })
	gotoCursor := widget.NewButton("⌖", func() { ide.session.GotoCursor(caretByte(ide.editor)) })
	checkAll := widget.NewButton("run", func() { ide.session.CheckAll() })
	controls := container.NewHBox(retract, advance, gotoCursor, checkAll)
	toolbar := container.NewBorder(nil, nil, nil, controls, ideStatus)

	return container.NewBorder(toolbar, ide.status, nil, nil, workspace)
}

func (ide *IDE) bindShortcuts() {
	ide.window.Canvas().AddShortcut(&desktop.CustomShortcut{KeyName: fyne.KeyDown, Modifier: fyne.KeyModifierShortcutDefault}, func(fyne.Shortcut) { ide.session.Advance() })
	ide.window.Canvas().AddShortcut(&desktop.CustomShortcut{KeyName: fyne.KeyUp, Modifier: fyne.KeyModifierShortcutDefault}, func(fyne.Shortcut) { ide.session.Retract() })
	ide.window.Canvas().AddShortcut(&desktop.CustomShortcut{KeyName: fyne.KeyRight, Modifier: fyne.KeyModifierShortcutDefault}, func(fyne.Shortcut) { ide.session.GotoCursor(caretByte(ide.editor)) })
	ide.window.Canvas().AddShortcut(&desktop.CustomShortcut{KeyName: fyne.KeyReturn, Modifier: fyne.KeyModifierShortcutDefault}, func(fyne.Shortcut) { ide.session.CheckAll() })
}

func (ide *IDE) render() {
	ide.renderStatus()
	ide.renderGutter()
	ide.panels.Render()
}

func (ide *IDE) renderGutter() {
	lines := strings.Count(ide.editor.Text, "\n") + 1
	var out strings.Builder
	for line := 0; line < lines; line++ {
		marker := "  "
		if ide.session.IsLocusLine(line) {
			marker = "> "
		}
		if ide.session.IsObligationLine(line) {
			marker = "• "
		}
		if ide.session.IsDiagnosticLine(line) {
			marker = "! "
		}
		fmt.Fprintf(&out, "%s%4d\n", marker, line+1)
	}
	ide.gutter.SetText(strings.TrimSuffix(out.String(), "\n"))
}

func (ide *IDE) renderStatus() {
	s := ide.session.State()
	caret := caretByte(ide.editor)
	line, col := positionAt(ide.editor.Text, caret)
	status := s.Status
	if status == "idle" {
		status = "accepted"
	}
	compiler := ide.compiler.Source
	if compiler == "" {
		compiler = "no emeraldc"
	}
	ide.status.SetText(fmt.Sprintf("%d:%d    locus %d/%d    check %d ms    %s    %s", line+1, col+1, s.Locus+1, len(s.Analysis.Statements), s.Millis, status, compiler))
}

func (ide *IDE) scrollToLine(line int) {
	lines := strings.Split(ide.editor.Text, "\n")
	if line < 0 || line >= len(lines) {
		return
	}
	prefix := strings.Join(lines[:line], "\n")
	ide.editor.CursorRow = line
	ide.editor.CursorColumn = 0
	_ = prefix
	ide.editor.Refresh()
}

func (ide *IDE) openFile() {
	dialog.ShowFileOpen(func(reader fyne.URIReadCloser, err error) {
		if err != nil || reader == nil {
			return
		}
		defer reader.Close()
		data, readErr := os.ReadFile(reader.URI().Path())
		if readErr != nil {
			dialog.ShowError(readErr, ide.window)
			return
		}
		ide.filePath = reader.URI().Path()
		ide.editor.SetText(string(data))
		ide.session.SetFileContext(filepath.Dir(ide.filePath), filepath.Base(ide.filePath))
		ide.session.SetText(string(data), 0, true)
		ide.render()
	}, ide.window)
}

func (ide *IDE) saveFile() {
	if ide.filePath == "" {
		ide.saveAs()
		return
	}
	if err := os.WriteFile(ide.filePath, []byte(ide.editor.Text), 0644); err != nil {
		dialog.ShowError(err, ide.window)
	}
}

func (ide *IDE) saveAs() {
	dialog.ShowFileSave(func(writer fyne.URIWriteCloser, err error) {
		if err != nil || writer == nil {
			return
		}
		defer writer.Close()
		ide.filePath = writer.URI().Path()
		if _, writeErr := writer.Write([]byte(ide.editor.Text)); writeErr != nil {
			dialog.ShowError(writeErr, ide.window)
			return
		}
		ide.session.SetFileContext(filepath.Dir(ide.filePath), filepath.Base(ide.filePath))
	}, ide.window)
}

func (ide *IDE) newFile() {
	ide.filePath = ""
	ide.editor.SetText("")
	ide.session.SetFileContext("", "untitled.rald")
	ide.session.SetText("", 0, true)
	ide.render()
}

func positionAt(text string, byteOffset int) (int, int) {
	if byteOffset < 0 {
		byteOffset = 0
	}
	if byteOffset > len(text) {
		byteOffset = len(text)
	}
	line, col := 0, 0
	for i := 0; i < byteOffset; i++ {
		if text[i] == '\n' {
			line++
			col = 0
		} else {
			col++
		}
	}
	return line, col
}

func caretByte(editor *widget.Entry) int {
	text := editor.Text
	row, col := editor.CursorRow, editor.CursorColumn
	lines := strings.Split(text, "\n")
	if row < 0 {
		row = 0
	}
	if row >= len(lines) {
		row = len(lines) - 1
	}
	pos := 0
	for i := 0; i < row; i++ {
		pos += len(lines[i]) + 1
	}
	if row >= 0 && row < len(lines) && col >= 0 {
		line := []rune(lines[row])
		if col > len(line) {
			col = len(line)
		}
		pos += len(string(line[:col]))
	}
	return pos
}

func main() {
	a := app.NewWithID("com.emerald.ide")
	w := a.NewWindow("Emerald IDE")
	ide := newIDE(a, w)
	w.SetMainMenu(fyne.NewMainMenu(
		fyne.NewMenu("File",
			fyne.NewMenuItem("New", ide.newFile),
			fyne.NewMenuItem("Open…", ide.openFile),
			fyne.NewMenuItem("Save", ide.saveFile),
			fyne.NewMenuItem("Save As…", ide.saveAs),
		),
		fyne.NewMenu("Session",
			fyne.NewMenuItem("Advance", ide.session.Advance),
			fyne.NewMenuItem("Retract", ide.session.Retract),
			fyne.NewMenuItem("Check to Cursor", func() { ide.session.GotoCursor(caretByte(ide.editor)) }),
			fyne.NewMenuItem("Check All", ide.session.CheckAll),
			fyne.NewMenuItem("Interrupt", ide.session.Interrupt),
		),
	))
	w.SetContent(ide.content())
	w.Resize(fyne.NewSize(1200, 760))
	w.SetMaster()
	w.SetCloseIntercept(func() { w.Close() })
	go func() {
		for range time.NewTicker(2 * time.Second).C {
			if ide.compiler.Path == "" {
				ide.compiler = ResolveCompiler()
				ide.renderStatus()
			}
		}
	}()
	w.ShowAndRun()
}
