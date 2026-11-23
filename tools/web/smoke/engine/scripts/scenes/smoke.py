import engine


def SceneStart():
    print("GATE3 SceneStart")
    engine.spawn_actor("Ball", "scripts/actors/ball.py", 480.0, 270.0)
    canvas = engine.ui.spawn_canvas("UI", "scripts/actors/ui.py", reference_width=960, reference_height=540)
    canvas.set_theme(engine.ui.modern_theme())
    button = canvas.add_button("Button")
    button.text = "Click me"
    button.anchor_min = button.anchor_max = engine.Vector2(0.5, 0.8)
    button.size_delta = engine.Vector2(220, 60)
    label = canvas.add_text("Label")
    label.anchor_min = label.anchor_max = engine.Vector2(0.5, 0.15)
    label.size_delta = engine.Vector2(700, 50)
