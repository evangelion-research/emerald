package main

import (
	"fmt"
	"strings"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/widget"
)

type PanelView struct {
	session *Session
	jump    func(int)
	tabs    *container.AppTabs
	content fyne.CanvasObject
	repl    *widget.Entry
}

func NewPanelView(session *Session, jump func(int)) *PanelView {
	p := &PanelView{session: session, jump: jump, repl: widget.NewEntry()}
	p.repl.SetPlaceHolder("expression...")
	p.repl.OnSubmitted = func(expr string) {
		if strings.TrimSpace(expr) == "" {
			return
		}
		p.runREPL(expr)
	}
	p.tabs = container.NewAppTabs(
		container.NewTabItem("goal", widget.NewLabel("")),
		container.NewTabItem("symbols", widget.NewLabel("")),
		container.NewTabItem("ledger", widget.NewLabel("")),
		container.NewTabItem("diagnostics", widget.NewLabel("")),
		container.NewTabItem("repl", container.NewBorder(nil, p.repl, nil, nil, widget.NewLabel(""))),
	)
	p.content = p.tabs
	return p
}
func (p *PanelView) Content() fyne.CanvasObject { return p.content }
func (p *PanelView) Render() {
	s := p.session.State()
	p.tabs.Items[0].Content = p.goal(s)
	p.tabs.Items[1].Content = p.symbols(s)
	p.tabs.Items[2].Content = p.ledger(s)
	p.tabs.Items[3].Content = p.diagnostics(s)
	p.tabs.Refresh()
}
func (p *PanelView) jumpButton(text string, line int) fyne.CanvasObject {
	b := widget.NewButton(text, func() { p.jump(line) })
	b.Alignment = widget.ButtonAlignLeading
	return b
}
func (p *PanelView) section(title string, objects ...fyne.CanvasObject) fyne.CanvasObject {
	head := widget.NewLabel(title)
	head.TextStyle = fyne.TextStyle{Bold: true, Monospace: true}
	return container.NewVBox(head, container.NewVBox(objects...))
}
func (p *PanelView) goal(s SessionState) fyne.CanvasObject {
	if len(s.Analysis.Statements) == 0 {
		return widget.NewLabel("No statements yet.")
	}
	items := []fyne.CanvasObject{widget.NewLabel("context"), widget.NewLabel("top level")}
	if s.Status == "failed" && len(s.Diags) > 0 {
		d := s.Diags[0]
		items = append(items, widget.NewLabel("residual goal"), widget.NewLabel(d.Message), widget.NewLabel(fmt.Sprintf("expected  %s\nactual    %s", d.Expected, d.Actual)))
	} else if s.Locus >= 0 {
		items = append(items, widget.NewLabel("residual goal"), widget.NewLabel(fmt.Sprintf("nothing to prove - %d/%d statements accepted", s.Locus+1, len(s.Analysis.Statements))))
	} else {
		items = append(items, widget.NewLabel("residual goal"), widget.NewLabel("advance into the buffer to start the session"))
	}
	env := []fyne.CanvasObject{widget.NewLabel(fmt.Sprintf("environment · %d", countAcceptedSymbols(s.Analysis.Symbols, s.Locus)))}
	for _, sym := range s.Analysis.Symbols {
		if sym.Stmt <= s.Locus {
			env = append(env, p.jumpButton(fmt.Sprintf("%s  %s  %s", sym.Kind, sym.Name, sym.Detail), s.Analysis.Statements[sym.Stmt].StartLine))
		}
	}
	return container.NewScroll(container.NewVBox(append(items, env...)...))
}
func countAcceptedSymbols(symbols []Symbol, locus int) int {
	n := 0
	for _, sym := range symbols {
		if sym.Stmt <= locus {
			n++
		}
	}
	return n
}
func (p *PanelView) symbols(s SessionState) fyne.CanvasObject {
	if len(s.Analysis.Symbols) == 0 {
		return widget.NewLabel("No symbols.")
	}
	rows := []fyne.CanvasObject{widget.NewLabel(fmt.Sprintf("%d symbols", len(s.Analysis.Symbols)))}
	for _, sym := range s.Analysis.Symbols {
		dim := ""
		if sym.Stmt > s.Locus {
			dim = " (unchecked)"
		}
		rows = append(rows, p.jumpButton(fmt.Sprintf("%s  %s  %s%s", sym.Kind, sym.Name, sym.Detail, dim), s.Analysis.Statements[sym.Stmt].StartLine))
	}
	return container.NewScroll(container.NewVBox(rows...))
}
func (p *PanelView) ledger(s SessionState) fyne.CanvasObject {
	if len(s.Analysis.Obligations) == 0 {
		return widget.NewLabel("No obligations. never bindings and negation functions appear here as you write them.")
	}
	proved, failed := 0, 0
	rows := []fyne.CanvasObject{}
	for _, o := range s.Analysis.Obligations {
		v := "unchecked"
		if o.Stmt <= s.Locus {
			v = "proved"
			proved++
		}
		if o.Stmt == s.Failing {
			v = "failed"
			failed++
		}
		rows = append(rows, p.jumpButton(fmt.Sprintf("%-9s %-14s %s :%d", v, o.Kind, o.Name, o.Line+1), o.Line))
	}
	summary := widget.NewLabel(fmt.Sprintf("%d proved · %d failed · %d unchecked", proved, failed, len(s.Analysis.Obligations)-proved-failed))
	return container.NewScroll(container.NewVBox(append([]fyne.CanvasObject{summary}, rows...)...))
}
func (p *PanelView) diagnostics(s SessionState) fyne.CanvasObject {
	if len(s.Diags) == 0 {
		if s.Status == "offline" {
			return widget.NewLabel("emeraldc not found - parse checks only.")
		}
		return widget.NewLabel("No diagnostics.")
	}
	rows := []fyne.CanvasObject{}
	for _, d := range s.Diags {
		rows = append(rows, p.jumpButton(fmt.Sprintf("%s · %s\n%s\nline %d, col %d", d.Severity, d.Code, d.Message, d.Line, d.Column), d.Line-1))
	}
	return container.NewScroll(container.NewVBox(rows...))
}
func (p *PanelView) runREPL(expr string) {
	result := p.session.EvalExpression(expr)
	label := widget.NewLabel(fmt.Sprintf("> %s\n%s", expr, replResult(result)))
	p.tabs.Items[4].Content = container.NewBorder(nil, p.repl, nil, nil, container.NewScroll(container.NewVBox(label)))
	p.tabs.Refresh()
}
func replResult(result CheckOutcome) string {
	if result.Error != "" {
		return result.Error
	}
	if len(result.Diags) > 0 {
		return result.Diags[0].Message
	}
	return fmt.Sprintf("accepted · %d ms", result.Millis)
}
