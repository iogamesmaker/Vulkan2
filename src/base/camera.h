#include "SDL3/SDL_events.h"
#include <vk_types.h>
#include <functional>
#include <limits>

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

	// walking mode: stays on the ground (groundHeight is fed in from the GPU every frame), gravity + jumping
	bool walking = false;
	float groundHeight = 0.0f;
	// flying: the terrain under the camera, the camera stays above it. under the map there's nothing
	// sensible to see (back faces). NaN = unknown yet
	float floorHeight = std::numeric_limits<float>::quiet_NaN();
	float metersPerUnit = 6.0f; // world scale, the walking numbers below are in metres
	float verticalVelocity = 0.0f;
	bool onGround = false;
	// moves the eye position's xz out of whatever it walked into and returns the ground under it, given the
	// feet height (set by the application, see collision.hpp). without it groundHeight is used as is
	std::function<float(glm::vec3& position, float feet)> collide;
	float eyeHeight = 1.7f, stepHeight = 0.5f; // metres
	float stepSmoothing = 0.0f; // world units the view lags behind sudden steps up / down, eased out

	bool spaceHeld = false; // for detecting presses instead of holding

	// zoom: hold C to zoom in (the mouse wheel sets how far while it's held). zoom = the current magnification,
	// spyglass = how far it's in (0..1, eased)
	float zoom = 1.0f;
	float zoomTarget = 8.0f; // 2x .. 64x
	float spyglass = 0.0f;
	bool forceSpyglass = false; // zoomed without the key (VULKAN2_SPYGLASS=<zoom>, for the captures)

    void processSDLEvent(SDL_Event& e);

    void update(float deltatime);
	void updateZoom(float deltatime);
};
