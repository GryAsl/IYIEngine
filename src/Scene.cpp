// Provides the compilation unit for the Scene class.
// A Scene in IYI Engine is a minimal container holding an ActorRegistry and an associated scene script path.
// It deliberately contains no complex scene graph logic, spatial acceleration structures, or serialization code;
// scene transitions are managed by Application, which transfers persistent actors and instantiates a new Scene.
//
// Connected to:
// - Implements declarations from include/IYI/Scene.h.
// - Instantiated and managed by Application (m_currentScene).
// - Contains the active ActorRegistry for the level.
//
// Future evolution (per ROADMAP.md):
// Scenes remain lightweight script-driven containers. Node hierarchies, prefab instantiation systems,
// and visual editor scene formats are explicitly rejected.

#include "IYI/Scene.h"

namespace IYI {
// Scene is a plain holder struct/class
} // namespace IYI
