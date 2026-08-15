import QtQuick

// NX design language living background: a deep space field with a slow
// drifting nebula and a sparse static starfield (nx-hub docs/DESIGN.md).
// The dashboard uses the software scene graph, so the drift is stepped by a
// 10 Hz timer instead of per-frame animations to keep repaints cheap, and it
// pauses entirely while the window is hidden.
Rectangle {
    id: bg

    gradient: Gradient {
        GradientStop { position: 0.0; color: "#0a0714" }
        GradientStop { position: 1.0; color: "#12091f" }
    }

    property real phase: 0 // seconds

    Timer {
        interval: 100
        repeat: true
        running: bg.visible && bg.Window.visibility !== Window.Hidden
        onTriggered: bg.phase = (bg.phase + 0.1) % 3600
    }

    component NebulaBlob: Canvas {
        property color tint: "#7700ff"
        property real strength: 0.1
        onPaint: {
            let ctx = getContext("2d");
            ctx.reset();
            let g = ctx.createRadialGradient(width / 2, height / 2, 0, width / 2, height / 2, width / 2);
            g.addColorStop(0.0, Qt.rgba(tint.r, tint.g, tint.b, strength));
            g.addColorStop(0.55, Qt.rgba(tint.r, tint.g, tint.b, strength * 0.35));
            g.addColorStop(1.0, Qt.rgba(tint.r, tint.g, tint.b, 0));
            ctx.fillStyle = g;
            ctx.fillRect(0, 0, width, height);
        }
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
    }

    // Violet, upper left: the light source of the scene
    NebulaBlob {
        width: bg.width * 1.15
        height: width
        x: -width * 0.35 + Math.sin(bg.phase * 2 * Math.PI / 96) * bg.width * 0.04
        y: -height * 0.45 + Math.cos(bg.phase * 2 * Math.PI / 96) * bg.height * 0.05
        tint: "#7700ff"
        strength: 0.13
    }

    // Cyan, lower right, drifting the opposite way
    NebulaBlob {
        width: bg.width * 0.9
        height: width
        x: bg.width - width * 0.55 - Math.sin(bg.phase * 2 * Math.PI / 74) * bg.width * 0.05
        y: bg.height - height * 0.5 + Math.cos(bg.phase * 2 * Math.PI / 74) * bg.height * 0.04
        tint: "#00e5ff"
        strength: 0.07
    }

    // Faint magenta accent, upper right
    NebulaBlob {
        width: bg.width * 0.7
        height: width
        x: bg.width * 0.45 + Math.sin(bg.phase * 2 * Math.PI / 110) * bg.width * 0.06
        y: bg.height * 0.2 - Math.cos(bg.phase * 2 * Math.PI / 110) * bg.height * 0.04
        tint: "#b03cff"
        strength: 0.05
    }

    // Sparse static starfield, seeded so it is identical on every launch
    Canvas {
        anchors.fill: parent
        onPaint: {
            let ctx = getContext("2d");
            ctx.reset();
            let seed = 7700;
            function rand() {
                seed = (seed * 1103515245 + 12345) % 2147483648;
                return seed / 2147483648;
            }
            ctx.fillStyle = "#efeaff";
            for (let i = 0; i < 140; ++i) {
                let sx = rand() * width;
                let sy = rand() * height;
                let r = 0.4 + rand() * 0.8;
                ctx.globalAlpha = 0.1 + rand() * 0.35;
                ctx.beginPath();
                ctx.arc(sx, sy, r, 0, 2 * Math.PI);
                ctx.fill();
            }
        }
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
    }
}
