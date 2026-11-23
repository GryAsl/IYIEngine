# Fixture actor: logs the engine's actor-start verification line, nothing else.
import engine


def Start(actor):
    engine.log_info("Start: actor #{} '{}'".format(actor.id, actor.name))
