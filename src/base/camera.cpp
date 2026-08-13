#include "camera.h"
#include "glm/gtx/quaternion.hpp"
#include "glm/gtx/transform.hpp"

glm::mat4 Camera::getViewMatrix(glm::vec3 offset) const
{
    // to create a correct model view, we need to move the world in opposite
    // direction to the camera
    //  so we will create the camera model matrix and invert
    glm::mat4 cameraTranslation = glm::translate(glm::mat4(1.f), position - offset);
    glm::mat4 cameraRotation = getRotationMatrix();
    return glm::inverse(cameraTranslation * cameraRotation);
}

glm::mat4 Camera::getRotationMatrix() const
{
    // fairly typical FPS style camera. we join the pitch and yaw rotations into
    // the final rotation matrix

    glm::quat pitchRotation = glm::angleAxis(pitch, glm::vec3 { 1.f, 0.f, 0.f });
    glm::quat yawRotation = glm::angleAxis(yaw, glm::vec3 { 0.f, -1.f, 0.f });

    return glm::toMat4(yawRotation) * glm::toMat4(pitchRotation);
}

void Camera::processSDLEvent(SDL_Event& e)
{
    if (e.type == SDL_EVENT_MOUSE_MOTION) {
        yaw += (float)e.motion.xrel / 150.f;
        pitch -= (float)e.motion.yrel / 150.f;
		
		if(pitch >  1.6) pitch =  1.6;
		if(pitch < -1.6) pitch = -1.6;
    }
}

void Camera::update(float deltatime)
{
    const bool* state = SDL_GetKeyboardState(NULL);

    glm::vec3 moveDir(0.0f);

    if (state[SDL_SCANCODE_W]) { moveDir.z -= 1.0f; }
    if (state[SDL_SCANCODE_S]) { moveDir.z += 1.0f; }
    if (state[SDL_SCANCODE_A]) { moveDir.x -= 1.0f; }
    if (state[SDL_SCANCODE_D]) { moveDir.x += 1.0f; }
    if (state[SDL_SCANCODE_Q]) { moveDir.y -= 1.0f; }
    if (state[SDL_SCANCODE_E]) { moveDir.y += 1.0f; }
    if (state[SDL_SCANCODE_MINUS])  { speed *= 0.9f; }
    if (state[SDL_SCANCODE_EQUALS]) { speed *= 1.1f; }
	speed = glm::clamp(speed, 0.01f, 20.f);

    if (glm::length(moveDir) > 0.0f) {
        moveDir = glm::normalize(moveDir);
    }

	float mul = 10.0f * speed;
	
	if(state[SDL_SCANCODE_LSHIFT]) mul *= 5.f;
    glm::mat4 cameraRotation = getRotationMatrix();
    position += glm::vec3(cameraRotation * glm::vec4(moveDir * mul * deltatime, 0.0f));
}