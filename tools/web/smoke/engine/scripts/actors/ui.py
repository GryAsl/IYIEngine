import engine


def Start(canvas):
    canvas.state["clicks"] = 0


def Update(canvas, dt):
    button = canvas.find("Button")
    if button and button.clicked:
        canvas.state["clicks"] += 1
        print(f"GATE3 click {canvas.state['clicks']}")
    label = canvas.find("Label")
    if label:
        label.text = f"clicks {canvas.state['clicks']}  {engine.get_screen_width()}x{engine.get_screen_height()}"
