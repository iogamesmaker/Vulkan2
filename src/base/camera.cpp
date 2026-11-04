#include "camera.h"
#include "glm/gtx/quaternion.hpp"
#include "glm/gtx/transform.hpp"

glm::mat4 Camera::getViewMatrix(glm::vec3 offset) const
{
    // to create a correct model view, we need to move the world in opposite
    // direction to the camera
    //  so we will create the camera model matrix and invert
    glm::mat4 cameraTranslation = glm::translate(glm::mat4(1.f), position - offset + glm::vec3(0.0f, stepSmoothing, 0.0f));
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
        // slower while zoomed, so the view moves the same on screen
        yaw += (float)e.motion.xrel / (150.f * zoom);
        pitch -= (float)e.motion.yrel / (150.f * zoom);
		
		if(pitch >  1.6) pitch =  1.6;
		if(pitch < -1.6) pitch = -1.6;
    }
	// the wheel sets the magnification while C is held
	if (e.type == SDL_EVENT_MOUSE_WHEEL && SDL_GetKeyboardState(NULL)[SDL_SCANCODE_C])
		zoomTarget = glm::clamp(zoomTarget * std::pow(1.25f, e.wheel.y), 2.0f, 64.0f);
}

// in / out over ~0.2s, the magnification eased in log space (so 1x -> 8x feels even all the way)
void Camera::updateZoom(float deltatime)
{
	bool held = SDL_GetKeyboardState(NULL)[SDL_SCANCODE_C] || forceSpyglass;
	spyglass += ((held ? 1.0f : 0.0f) - spyglass) * (1.0f - std::exp(-deltatime * 14.0f));
	if (std::abs(spyglass - (held ? 1.0f : 0.0f)) < 1e-3f) spyglass = held ? 1.0f : 0.0f;
	float t = spyglass * spyglass * (3.0f - 2.0f * spyglass);
	zoom = std::exp(std::log(zoomTarget) * t);
}

void Camera::update(float deltatime)
{
    const bool* state = SDL_GetKeyboardState(NULL);
	updateZoom(deltatime);

	if (walking) {
		const float walkSpeed = 2.0f, sprintSpeed = 7.0f, gravity = 9.81f, jumpSpeed = 4.5f;
		float toUnits = 1.0f / metersPerUnit;

		// move in the horizontal plane only, looking up/down doesn't make you fly
		glm::vec3 moveDir(0.0f);
		if (state[SDL_SCANCODE_W]) { moveDir.z -= 1.0f; }
		if (state[SDL_SCANCODE_S]) { moveDir.z += 1.0f; }
		if (state[SDL_SCANCODE_A]) { moveDir.x -= 1.0f; }
		if (state[SDL_SCANCODE_D]) { moveDir.x += 1.0f; }
		if (glm::length(moveDir) > 0.0f) moveDir = glm::normalize(moveDir);

		float horizontalSpeed = (state[SDL_SCANCODE_LSHIFT] ? sprintSpeed : walkSpeed) * toUnits;
		glm::mat4 yawRotation = glm::toMat4(glm::angleAxis(yaw, glm::vec3 { 0.f, -1.f, 0.f }));
		position += glm::vec3(yawRotation * glm::vec4(moveDir * horizontalSpeed * deltatime, 0.0f));

		bool spacePressed = state[SDL_SCANCODE_SPACE] && !spaceHeld;
		spaceHeld = state[SDL_SCANCODE_SPACE];
		float eye = eyeHeight * toUnits;
		if (collide) groundHeight = collide(position, position.y - eye);
		float target = groundHeight + eye;

		if (spacePressed && onGround) { verticalVelocity = jumpSpeed * toUnits; onGround = false; }
		verticalVelocity -= gravity * toUnits * deltatime;
		position.y += verticalVelocity * deltatime;

		bool wasOnGround = onGround;
		// walking downhill / off a rock: stay on the ground instead of skipping through the air
		bool stickDown = wasOnGround && verticalVelocity <= 0.0f && position.y > target && position.y - target < stepHeight * toUnits;
		onGround = position.y <= target || stickDown;
		if (onGround) {
			// steps (onto a rock, a log, down from one) ease the view instead of snapping it
			if (wasOnGround) stepSmoothing += position.y - target;
			position.y = target;
			verticalVelocity = 0.0f;
		}
		stepSmoothing *= std::exp(-deltatime * 14.0f);
		stepSmoothing = glm::clamp(stepSmoothing, -stepHeight * toUnits, stepHeight * toUnits);
		return;
	}

    stepSmoothing = 0.0f;
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
	if (!std::isnan(floorHeight)) position.y = std::max(position.y, floorHeight + 0.5f / metersPerUnit); // not under the map
}