package main

import (
	"fmt"
	"math"
	"os"
	"os/signal"
	"syscall"
	"time"
)

const (
	Width  = 64
	Height = 18
	Size   = Width * Height
	// Fits the 960x540 VHS terminal at 18px without wrapping or scrolling.
	ScaleX = 85.0
	ScaleY = 25.0
)

type Vec3 struct {
	X, Y, Z float64
}

type Face struct {
	A, B, C int
}

var vertices = []Vec3{
	{0.0, 1.6, 0.0},
	{-1.0, 0.6, -0.7},
	{1.0, 0.6, -0.7},
	{1.0, 0.6, 0.7},
	{-1.0, 0.6, 0.7},
	{-0.8, -0.6, -0.55},
	{0.8, -0.6, -0.55},
	{0.8, -0.6, 0.55},
	{-0.8, -0.6, 0.55},
	{0.0, -1.6, 0.0},
}

var faces = []Face{
	{0, 1, 2},
	{0, 2, 3},
	{0, 3, 4},
	{0, 4, 1},
	{1, 5, 6},
	{1, 6, 2},
	{2, 6, 7},
	{2, 7, 3},
	{3, 7, 8},
	{3, 8, 4},
	{4, 8, 5},
	{4, 5, 1},
	{9, 6, 5},
	{9, 7, 6},
	{9, 8, 7},
	{9, 5, 8},
}

var (
	buffer  [Size]byte
	zbuffer [Size]float64
)

func sub(a, b Vec3) Vec3 {
	return Vec3{a.X - b.X, a.Y - b.Y, a.Z - b.Z}
}

func cross(a, b Vec3) Vec3 {
	return Vec3{
		a.Y*b.Z - a.Z*b.Y,
		a.Z*b.X - a.X*b.Z,
		a.X*b.Y - a.Y*b.X,
	}
}

func dot(a, b Vec3) float64 {
	return a.X*b.X + a.Y*b.Y + a.Z*b.Z
}

func normalize(v Vec3) Vec3 {
	length := math.Sqrt(dot(v, v))
	if length == 0.0 {
		return v
	}
	return Vec3{v.X / length, v.Y / length, v.Z / length}
}

func rotate(p Vec3, A, B, C float64) Vec3 {
	ca := math.Cos(A)
	sa := math.Sin(A)
	cb := math.Cos(B)
	sb := math.Sin(B)
	cc := math.Cos(C)
	sc := math.Sin(C)
	// rotate around X
	{
		y := p.Y*ca - p.Z*sa
		z := p.Y*sa + p.Z*ca

		p.Y = y
		p.Z = z
	}
	// rotate around Y
	{
		x := p.X*cb + p.Z*sb
		z := -p.X*sb + p.Z*cb

		p.X = x
		p.Z = z
	}
	// rotate around Z
	{
		x := p.X*cc - p.Y*sc
		y := p.X*sc + p.Y*cc

		p.X = x
		p.Y = y
	}
	return p
}

func clearScreenBuffers() {
	for i := range buffer {
		buffer[i] = ' '
	}
	for i := range zbuffer {
		zbuffer[i] = 0.0
	}
}

func drawSample(p Vec3, luminance float64) {
	const shades = ".,-~:;=!*#$@"
	shadeCount := len(shades) - 2
	const cameraDistance = 5.0
	z := p.Z + cameraDistance
	if z <= 0.1 {
		return
	}
	invZ := 1.0 / z
	// Terminal characters are taller than they are wide, so
	// X gets stretched a bit to compensate.
	screenX := Width/2 + int(ScaleX*p.X*invZ)
	screenY := Height/2 - int(ScaleY*p.Y*invZ)
	if screenX < 0 || screenX >= Width || screenY < 0 || screenY >= Height {
		return
	}
	index := screenX + screenY*Width
	if invZ <= zbuffer[index] {
		return
	}
	zbuffer[index] = invZ
	if luminance < 0.0 {
		luminance = 0.0
	}
	if luminance > 1.0 {
		luminance = 1.0
	}
	shade := int(luminance * float64(shadeCount))
	buffer[index] = shades[shade]
}

func renderTriangle(a, b, c Vec3, A, B, C float64) {
	// Rotate triangle vertices.
	ra := rotate(a, A, B, C)
	rb := rotate(b, A, B, C)
	rc := rotate(c, A, B, C)
	// Surface normal.
	edge1 := sub(rb, ra)
	edge2 := sub(rc, ra)
	normal := normalize(cross(edge1, edge2))
	// Light coming from slightly above, left and toward viewer.
	light := normalize(Vec3{-0.5, 1.0, -1.0})
	// The camera looks along +Z, so only outward faces toward -Z are visible.
	if normal.Z >= 0.0 {
		return
	}
	// Keep shadowed facets visible instead of cutting holes in the gem.
	diffuse := dot(normal, light)
	if diffuse < 0.0 {
		diffuse = 0.0
	}
	luminance := 0.18 + 0.82*diffuse
	// Sample points across the triangle using barycentric coordinates.
	//
	// p = a + u(b-a) + v(c-a)
	//
	// with:
	//   u >= 0
	//   v >= 0
	//   u + v <= 1
	//
	// Dense sampling keeps the enlarged facets solid instead of speckled.
	const step = 0.0125
	for u := 0.0; u <= 1.0; u += step {
		for v := 0.0; v <= 1.0-u; v += step {
			p := Vec3{
				ra.X + u*(rb.X-ra.X) + v*(rc.X-ra.X),
				ra.Y + u*(rb.Y-ra.Y) + v*(rc.Y-ra.Y),
				ra.Z + u*(rb.Z-ra.Z) + v*(rc.Z-ra.Z),
			}
			// Add a small variation across the facet.
			//
			// This gives the gem a little more visual texture
			// while preserving the flat-facet lighting.
			edgeGlow := 0.15 * math.Sin(u*15.0+v*10.0)
			brightness := luminance + edgeGlow
			if brightness < 0.0 {
				brightness = 0.0
			}
			if brightness > 1.0 {
				brightness = 1.0
			}
			drawSample(p, brightness)
		}
	}
}

func render(A, B, C float64) {
	for _, f := range faces {
		renderTriangle(
			vertices[f.A],
			vertices[f.B],
			vertices[f.C],
			A, B, C,
		)
	}
}

func cleanup() {
	fmt.Print("\x1b[0m")
	fmt.Print("\x1b[?25h")
	os.Exit(0)
}

func printFrame() {
	// ANSI:
	// \x1b[H = cursor home
	// 38;2;... = RGB foreground color
	//
	// This makes the ASCII emerald actually emerald green.
	var out []byte
	out = append(out, []byte("\x1b[H")...)
	out = append(out, []byte("\x1b[38;2;20;220;120m")...)
	for y := 0; y < Height; y++ {
		for x := 0; x < Width; x++ {
			index := x + y*Width
			out = append(out, buffer[index])
		}
		out = append(out, '\n')
	}
	out = append(out, []byte("\x1b[0m")...)
	os.Stdout.Write(out)
}

func main() {
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)
	go func() {
		<-sigCh
		cleanup()
	}()
	A := 0.3
	B := 0.0
	C := 0.0
	// Clear terminal.
	fmt.Print("\x1b[2J")
	// Hide cursor.
	fmt.Print("\x1b[?25l")
	for {
		clearScreenBuffers()
		render(A, B, C)
		printFrame()
		// Similar idea to donut.c:
		// continuously modify rotation angles.
		A += 0.008
		B += 0.025
		C += 0.004
		// ~60 FPS
		time.Sleep(16 * time.Millisecond)
	}
}
