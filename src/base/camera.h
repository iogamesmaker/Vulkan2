#include "SDL3/SDL_events.h"
#include <vk_types.h>

class Camera {
public:
    glm::vec3 velocity;
    glm::vec3 position;
    // vertical rotation
    float pitch { 0.f };
    // horizontal rotation
    float yaw { 0.f };

    glm::mat4 getViewMatrix(glm::vec3 offset = {0.0, 0.0, 0.0} ) const;
    glm::mat4 getRotationMatrix() const;
	
	float speed = 1.0f;

    void processSDLEvent(SDL_Event& e);

    void update(float deltatime);
};
