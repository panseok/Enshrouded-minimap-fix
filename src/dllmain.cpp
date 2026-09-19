#include "pch.h"

#include <shroudtopia.h>

#if defined(_MSC_VER)
#pragma comment(lib, "gdi32.lib")
#endif
#include <memory_utils.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    constexpr uintptr_t RVA_LOCAL_PLAYER_UI_RENDER_SETUP = 0x300390;
    constexpr uintptr_t RVA_PLAYER_WAYPOINTS_UI = 0x29CFF0;
    constexpr uintptr_t RVA_KNOWLEDGE_QUERY_MAPMARKER_VISIBILITY = 0x381350;
    constexpr uintptr_t RVA_KNOWLEDGE_QUERY_MAPMARKER_VISIBILITY_LOOP = RVA_KNOWLEDGE_QUERY_MAPMARKER_VISIBILITY + 0x37;
    constexpr uintptr_t RVA_RENDER_PRESENT_FRAME = 0xDCFAE0;
    // ECS system "fog_of_war" (components FogOfWarDiscovery, PlayerState, RenderTransform,
    // LocalPlayerData, FogOfWar): marks the cells around the local player as discovered.
    constexpr uintptr_t RVA_FOG_OF_WAR = 0x2A3EC0;
    constexpr uintptr_t RVA_VULKAN_DEVICE_TABLE_INIT = 0xDDBDE0;
    constexpr uintptr_t RVA_ITER_INIT = 0x8CD000;
    constexpr uintptr_t RVA_ITER_NEXT = 0x8C84E0;
    constexpr uintptr_t HOOK_PATTERN_SCAN_RADIUS = 0x400000;

    constexpr std::size_t LOCAL_PLAYER_UI_RENDER_SETUP_STOLEN_SIZE = 13;
    constexpr std::size_t PLAYER_WAYPOINTS_UI_STOLEN_SIZE = 15;
    constexpr std::size_t KNOWLEDGE_QUERY_MAPMARKER_VISIBILITY_STOLEN_SIZE = 11;
    constexpr std::size_t KNOWLEDGE_QUERY_MAPMARKER_VISIBILITY_LOOP_STOLEN_SIZE = 5;
    constexpr std::size_t RENDER_PRESENT_FRAME_STOLEN_SIZE = 16;
    constexpr std::size_t FOG_OF_WAR_STOLEN_SIZE = 17;
    constexpr std::size_t VULKAN_DEVICE_TABLE_INIT_STOLEN_SIZE = 15;
    constexpr std::size_t UI_RENDER_SETUP_SOURCE_OFFSET = 0x10;
    constexpr std::size_t UI_RENDER_SETUP_STATE_OFFSET = 0x20;
    constexpr std::size_t WAYPOINT_STATE_PTR_OFFSET = 0x10;
    constexpr std::size_t WAYPOINT_ARRAY_OFFSET = 0x2FD390;
    constexpr std::size_t WAYPOINT_COUNT_OFFSET = 0x2FD398;
    // May-24-2026 client: verified live against the running game (custom-marker count
    // incremented at this offset when the player placed a map marker).
    constexpr std::size_t WAYPOINT_ARRAY_OFFSET_MAY24 = 0x3C128;
    constexpr std::size_t WAYPOINT_ENTRY_STRIDE = 0xF0;
    constexpr std::size_t WAYPOINT_ENTRY_STRIDE_MAY24 = 0x80;
    constexpr std::size_t WAYPOINT_ENTRY_POSITION_MAY24 = 0x10;
    // May-24-2026 client: live player position triple inside the UI state (verified to
    // track the player smoothly while moving).
    constexpr std::size_t UI_STATE_PLAYER_POSITION_MAY24 = 0x1DCB8;
    // May-24-2026 client: nearby/streamed marker array (float xyz at entry+0x0C).
    constexpr std::size_t NEARBY_ARRAY_OFFSET_MAY24 = 0x8E8;
    constexpr std::size_t NEARBY_ENTRY_STRIDE_MAY24 = 0x180;
    constexpr std::size_t NEARBY_ENTRY_POS_FLOAT_MAY24 = 0x0C;
    // May-24-2026 client: master world-map marker array — the same list the big map
    // renders (POIs, placed flame altars, NPCs, multiplayer pings). Verified live:
    // 66 entries matched the big map exactly, icon keys overlap the icon atlas.
    constexpr std::size_t MASTER_MARKER_ARRAY_OFFSET_MAY24 = 0x1C100;
    constexpr std::size_t MASTER_MARKER_STRIDE = 0x80;
    constexpr std::size_t MASTER_MARKER_POS_OFFSET = 0x10;
    constexpr std::size_t MASTER_MARKER_KEY_OFFSET = 0x30;
    constexpr std::size_t MASTER_MARKER_MAX_ENTRIES = 1024;
    // The UI state holds four identical marker arrays (0x80-byte entries, capacity 0x400),
    // set up together in the UI state constructor (exe 0x140766fe3). 0x3C128 is the
    // waypoint list; the other three all feed the world map with the same entry layout.
    constexpr std::size_t MARKER_ARRAY_OFFSETS_SEP26[] = { MASTER_MARKER_ARRAY_OFFSET_MAY24, 0x5C150, 0x7C178 };
    constexpr std::uint32_t MASTER_KEY_FLAME_ALTAR = 0xA447CBA3;
    constexpr std::uint32_t MASTER_KEY_PLAYER_PING = 0x813080BC;
    constexpr std::size_t WAYPOINT_ENTRY_POSITION_OFFSET = 0x30;
    constexpr std::size_t WAYPOINT_ENTRY_ACTIVE_OFFSET = 0x48;
    // May-24-2026 client: UI-state camera/anchor block shifted -0x300 (0x16158 -> 0x15E58),
    // visible in local_player_ui_render_setup writes at [state+0x15E58..].
    constexpr std::size_t UI_STATE_CAMERA_BLOCK_OLD = 0x16158;
    constexpr std::size_t UI_STATE_CAMERA_BLOCK_NEW = 0x15E58;
    constexpr std::size_t UI_STATE_BIG_OBJECT_PROBE = 0x15E00;
    constexpr std::size_t NEARBY_MARKER_ARRAY_OFFSET = 0x3698;
    constexpr std::size_t NEARBY_MARKER_COUNT_OFFSET = 0x36A0;
    constexpr std::size_t INPUT_MARKER_ARRAY_OFFSET = 0x18;
    constexpr std::size_t INPUT_MARKER_COUNT_OFFSET = 0x28;
    constexpr std::size_t NEARBY_MARKER_ENTRY_STRIDE = 0x28;
    constexpr std::size_t DYNAMIC_MARKER_SCAN_BYTES = 0x90;
    constexpr std::size_t WAYPOINT_MAX_ENTRIES = 1024;
    constexpr std::size_t NEARBY_MARKER_MAX_ENTRIES = 2048;
    constexpr std::size_t VISIBLE_MAP_MARKER_MAX_ENTRIES = 2048;
    constexpr DWORD VISIBLE_MAP_MARKER_STALE_MS = 180000;
    constexpr DWORD VISIBLE_MAP_MARKER_PRUNE_MS = 2000;
    constexpr std::size_t VULKAN_TABLE_QUEUE_SUBMIT_OFFSET = 0xB8;
    constexpr std::size_t VULKAN_TABLE_GET_DEVICE_PROC_ADDR_OFFSET = 0x78;
    constexpr std::size_t VULKAN_TABLE_CREATE_SWAPCHAIN_OFFSET = 0x570;
    constexpr std::size_t VULKAN_TABLE_GET_SWAPCHAIN_IMAGES_OFFSET = 0x580;
    constexpr std::size_t VULKAN_TABLE_ACQUIRE_NEXT_IMAGE_OFFSET = 0x588;
    constexpr std::size_t VULKAN_TABLE_QUEUE_PRESENT_OFFSET = 0x590;
    constexpr std::size_t VULKAN_DEVICE_TABLE_SCAN_LIMIT = 0x900;
    constexpr DWORD WORLD_DATA_STALE_MS = 15000;
    constexpr DWORD SESSION_LOG_POLL_MS = 1000;
    constexpr DWORD SESSION_LOG_TAIL_BYTES = 128 * 1024;
    constexpr DWORD MINIMAP_CONFIG_POLL_MS = 1000;
    constexpr int REAL_MAP_MIN_TEXTURE_SIZE = 512;
    // 8192 = 1.25 world units per texel. The GPU map path samples it with trilinear
    // filtering, so large maps no longer cost anything per frame on the CPU.
    constexpr int REAL_MAP_MAX_TEXTURE_SIZE = 8192;
    constexpr float REAL_MAP_WORLD_SIZE = 10240.0f;
    constexpr std::size_t STATIC_POI_ENTRY_SIZE = 16;
    constexpr std::size_t STATIC_POI_MAX_ENTRIES = 4096;
    constexpr std::size_t STATIC_POI_MAX_DRAWN = 96;
    constexpr float MINIMAP_BASE_UNITS_PER_PIXEL = 3.25f;
    constexpr float STATIC_POI_DRAW_RADIUS_FACTOR = 0.78f;
    constexpr int MINIMAP_ICON_MIN_SIZE = 8;
    constexpr int MINIMAP_ICON_MAX_SIZE = 256;
    constexpr std::size_t MINIMAP_ICON_MAX_ENTRIES = 256;
    constexpr int MINIMAP_FRAME_MIN_SIZE = 128;
    constexpr int MINIMAP_FRAME_MAX_SIZE = 1024;
    constexpr std::uint32_t MAP_MARKER_QUEST_IMPORTANT_KIND = 0xA7CADC08u;
    constexpr std::uint32_t MAP_MARKER_QUEST_KIND = 0xC52E92E5u;
    constexpr int MINIMAP_DEFAULT_MAP_SAMPLE_STEP = 2;
    constexpr int MINIMAP_DEFAULT_MAX_DRAWN_POINTS = 64;

    constexpr double FIXED_32_32_TO_WORLD = 1.0 / 4294967296.0;

    constexpr std::int32_t VK_SUCCESS = 0;
    constexpr std::uint32_t VK_STRUCTURE_TYPE_SUBMIT_INFO = 4;
    constexpr std::uint32_t VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO = 9;
    constexpr std::uint32_t VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO = 15;
    constexpr std::uint32_t VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO = 37;
    constexpr std::uint32_t VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO = 38;
    constexpr std::uint32_t VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO = 39;
    constexpr std::uint32_t VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO = 40;
    constexpr std::uint32_t VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO = 42;
    constexpr std::uint32_t VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO = 43;
    // 45 per vulkan_core.h (46 is VK_STRUCTURE_TYPE_MEMORY_BARRIER).
    constexpr std::uint32_t VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER = 45;
    constexpr std::uint32_t VK_IMAGE_VIEW_TYPE_2D = 1;
    constexpr std::uint32_t VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO = 5;
    constexpr std::uint32_t VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO = 12;
    constexpr std::uint32_t VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO = 14;
    constexpr std::uint32_t VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO = 16;
    constexpr std::uint32_t VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO = 18;
    constexpr std::uint32_t VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO = 19;
    constexpr std::uint32_t VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO = 20;
    constexpr std::uint32_t VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO = 22;
    constexpr std::uint32_t VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO = 23;
    constexpr std::uint32_t VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO = 24;
    constexpr std::uint32_t VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO = 26;
    constexpr std::uint32_t VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO = 28;
    constexpr std::uint32_t VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO = 30;
    constexpr std::uint32_t VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO = 31;
    constexpr std::uint32_t VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO = 32;
    constexpr std::uint32_t VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO = 33;
    constexpr std::uint32_t VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO = 34;
    constexpr std::uint32_t VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET = 35;
    constexpr std::uint32_t VK_COMPONENT_SWIZZLE_IDENTITY = 0;
    constexpr std::uint32_t VK_FORMAT_R8G8B8A8_UNORM = 37;
    constexpr std::uint32_t VK_IMAGE_ASPECT_COLOR_BIT = 0x1;
    // 1 per vulkan_core.h (0 is VK_IMAGE_TYPE_1D).
    constexpr std::uint32_t VK_IMAGE_TYPE_2D = 1;
    constexpr std::uint32_t VK_IMAGE_TILING_OPTIMAL = 0;
    constexpr std::uint32_t VK_IMAGE_USAGE_TRANSFER_DST_BIT = 0x2;
    constexpr std::uint32_t VK_IMAGE_USAGE_SAMPLED_BIT = 0x4;
    constexpr std::uint32_t VK_BUFFER_USAGE_TRANSFER_SRC_BIT = 0x1;
    constexpr std::uint32_t VK_SAMPLE_COUNT_1_BIT = 0x1;
    constexpr std::uint32_t VK_ATTACHMENT_LOAD_OP_LOAD = 0;
    constexpr std::uint32_t VK_ATTACHMENT_STORE_OP_STORE = 0;
    constexpr std::uint32_t VK_ATTACHMENT_LOAD_OP_DONT_CARE = 2;
    constexpr std::uint32_t VK_ATTACHMENT_STORE_OP_DONT_CARE = 1;
    constexpr std::uint32_t VK_IMAGE_LAYOUT_UNDEFINED = 0;
    constexpr std::uint32_t VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL = 2;
    constexpr std::uint32_t VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL = 5;
    // 7 per vulkan_core.h (6 is VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL).
    constexpr std::uint32_t VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL = 7;
    constexpr std::uint32_t VK_IMAGE_LAYOUT_PRESENT_SRC_KHR = 1000001002;
    constexpr std::uint32_t VK_PIPELINE_BIND_POINT_GRAPHICS = 0;
    constexpr std::uint32_t VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT = 0x2;
    constexpr std::uint32_t VK_COMMAND_BUFFER_LEVEL_PRIMARY = 0;
    constexpr std::uint32_t VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT = 0x1;
    constexpr std::uint32_t VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT = 0x1;
    constexpr std::uint32_t VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT = 0x80;
    constexpr std::uint32_t VK_PIPELINE_STAGE_TRANSFER_BIT = 0x1000;
    constexpr std::uint32_t VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT = 0x400;
    constexpr std::uint32_t VK_ACCESS_TRANSFER_WRITE_BIT = 0x1000;
    constexpr std::uint32_t VK_ACCESS_SHADER_READ_BIT = 0x20;
    constexpr std::uint32_t VK_SHARING_MODE_EXCLUSIVE = 0;
    constexpr std::uint32_t VK_FILTER_LINEAR = 1;
    constexpr std::uint32_t VK_SAMPLER_MIPMAP_MODE_LINEAR = 1;
    constexpr std::uint32_t VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE = 2;
    constexpr std::uint32_t VK_BORDER_COLOR_INT_OPAQUE_BLACK = 0;
    constexpr std::uint32_t VK_COMPARE_OP_ALWAYS = 7;
    constexpr std::uint32_t VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER = 1;
    constexpr std::uint32_t VK_SHADER_STAGE_VERTEX_BIT = 0x1;
    constexpr std::uint32_t VK_SHADER_STAGE_FRAGMENT_BIT = 0x10;
    constexpr std::uint32_t VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST = 3;
    constexpr std::uint32_t VK_POLYGON_MODE_FILL = 0;
    constexpr std::uint32_t VK_CULL_MODE_NONE = 0;
    constexpr std::uint32_t VK_FRONT_FACE_COUNTER_CLOCKWISE = 1;
    constexpr std::uint32_t VK_COLOR_COMPONENT_R_BIT = 0x1;
    constexpr std::uint32_t VK_COLOR_COMPONENT_G_BIT = 0x2;
    constexpr std::uint32_t VK_COLOR_COMPONENT_B_BIT = 0x4;
    constexpr std::uint32_t VK_COLOR_COMPONENT_A_BIT = 0x8;
    constexpr std::uint32_t VK_BLEND_FACTOR_ZERO = 0;
    constexpr std::uint32_t VK_BLEND_FACTOR_ONE = 1;
    constexpr std::uint32_t VK_BLEND_FACTOR_SRC_ALPHA = 6;
    constexpr std::uint32_t VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA = 7;
    constexpr std::uint32_t VK_BLEND_OP_ADD = 0;
    constexpr std::uint32_t VK_LOGIC_OP_COPY = 3;

    using IterInitFn = void(__fastcall*)(void* ctx, void* buffer, std::uint32_t size);
    using IterNextFn = bool(__fastcall*)(void* ctx, void* buffer, std::uint32_t size);

    struct UiRenderSetupIterationRecord
    {
        void* unknown0 = nullptr;
        void* local30 = nullptr;
        std::uint8_t* source = nullptr;
        void* entity = nullptr;
        std::uint8_t* state = nullptr;
    };
    static_assert(sizeof(UiRenderSetupIterationRecord) == 0x28, "Unexpected local_player_ui_render_setup iteration record size");
    static_assert(offsetof(UiRenderSetupIterationRecord, source) == UI_RENDER_SETUP_SOURCE_OFFSET, "Unexpected UI render source offset");
    static_assert(offsetof(UiRenderSetupIterationRecord, state) == UI_RENDER_SETUP_STATE_OFFSET, "Unexpected UI render state offset");

    struct WaypointsUiIterationRecord
    {
        void* unknown0 = nullptr;
        // May-24-2026 client fills only two qwords here (iter record size 0x10): state is at +0x08.
        std::uint8_t* stateNew = nullptr;
        std::uint8_t* state = nullptr;
        void* lookupContext = nullptr;
        void* waypointList = nullptr;
        void* playerList = nullptr;
    };
    static_assert(sizeof(WaypointsUiIterationRecord) == 0x30, "Unexpected player_waypoints_ui iteration record size");
    static_assert(offsetof(WaypointsUiIterationRecord, state) == WAYPOINT_STATE_PTR_OFFSET, "Unexpected state offset");

    // fog_of_war (exe 0x1402a3ec0) iterates with a 0x30-byte record: [ctx, FogOfWarDiscovery*,
    // PlayerState*, RenderTransform*, LocalPlayerData*, FogOfWar*]. The FogOfWar object:
    //   +0x00 float mapSizeX, +0x04 float mapSizeZ      (world units)
    //   +0x08 u32 widthBlocks, +0x0C u32 heightBlocks   (blocks of 32x32 cells, 8 units per cell)
    //   +0x18 u8* blocks (widthBlocks*heightBlocks*1024, row-major inside a block)
    //   +0x20 u64 blockCount, +0x28.. dirty bitset, +0x40 u32 dirty count
    // A cell value is 0..255 ("how well discovered"), row 0 lies at z = mapSizeZ.
    struct FogOfWarIterationRecord
    {
        void* ctx = nullptr;
        void* discovery = nullptr;
        void* playerState = nullptr;
        void* renderTransform = nullptr;
        void* localPlayerData = nullptr;
        std::uint8_t* fogOfWar = nullptr;
    };
    static_assert(sizeof(FogOfWarIterationRecord) == 0x30, "Unexpected fog_of_war iteration record size");

    struct MapMarkerVisibilityIterationRecord
    {
        void* unknown0 = nullptr;
        std::uint8_t* marker = nullptr;
        std::uint8_t* visibilityState = nullptr;
        void* refCounter = nullptr;
        void* knowledge = nullptr;
        void* entity = nullptr;
    };
    static_assert(sizeof(MapMarkerVisibilityIterationRecord) == 0x30, "Unexpected map marker visibility iteration record size");

    struct CapturedWaypoint
    {
        std::uint64_t raw[8] = {};
        std::uint32_t id = 0;
        std::int64_t x = 0;
        std::int64_t y = 0;
        std::int64_t z = 0;
    };

    struct CapturedNearbyMarker
    {
        std::uint64_t raw[5] = {};
        std::int64_t x = 0;
        std::int64_t y = 0;
        std::int64_t z = 0;
        std::uint32_t kind = 0;
        bool hasWorldPosition = false;
    };

    struct CapturedMapMarkerVisibility
    {
        std::uint64_t raw[5] = {};
        uintptr_t markerAddress = 0;
        uintptr_t stateAddress = 0;
        uintptr_t entityAddress = 0;
        std::int64_t x = 0;
        std::int64_t y = 0;
        std::int64_t z = 0;
        std::uint32_t markerId = 0;
        std::uint32_t markerType = 0;
        std::uint32_t kind = 0;
        std::uint8_t visibility = 0;
        DWORD lastUpdateTick = 0;
        bool hasWorldPosition = false;
    };

    struct CapturedPlayerPosition
    {
        std::int64_t x = 0;
        std::int64_t y = 0;
        std::int64_t z = 0;
        uintptr_t source = 0;
        std::uint32_t offset = 0;
        std::uint32_t channel = 0;
        float headingRadians = 0.0f;
        DWORD lastUpdateTick = 0;
        bool hasHeading = false;
        bool valid = false;
    };

    struct VkExtent2D
    {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };

    struct VkExtent3D
    {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t depth = 1;
    };

    struct VkOffset2D
    {
        std::int32_t x = 0;
        std::int32_t y = 0;
    };

    struct VkOffset3D
    {
        std::int32_t x = 0;
        std::int32_t y = 0;
        std::int32_t z = 0;
    };

    struct VkRect2D
    {
        VkOffset2D offset;
        VkExtent2D extent;
    };

    struct VkSwapchainCreateInfoKHR
    {
        std::uint32_t sType;
        const void* pNext;
        std::uint32_t flags;
        void* surface;
        std::uint32_t minImageCount;
        std::uint32_t imageFormat;
        std::uint32_t imageColorSpace;
        VkExtent2D imageExtent;
        std::uint32_t imageArrayLayers;
        std::uint32_t imageUsage;
        std::uint32_t imageSharingMode;
        std::uint32_t queueFamilyIndexCount;
        const std::uint32_t* pQueueFamilyIndices;
        std::uint32_t preTransform;
        std::uint32_t compositeAlpha;
        std::uint32_t presentMode;
        std::uint32_t clipped;
        void* oldSwapchain;
    };

    struct VkPresentInfoKHR
    {
        std::uint32_t sType;
        const void* pNext;
        std::uint32_t waitSemaphoreCount;
        const void* const* pWaitSemaphores;
        std::uint32_t swapchainCount;
        const void* const* pSwapchains;
        const std::uint32_t* pImageIndices;
        std::int32_t* pResults;
    };
    static_assert(offsetof(VkPresentInfoKHR, pSwapchains) == 0x28, "Unexpected VkPresentInfoKHR pSwapchains offset");
    static_assert(offsetof(VkPresentInfoKHR, pImageIndices) == 0x30, "Unexpected VkPresentInfoKHR pImageIndices offset");

    struct VkComponentMapping
    {
        std::uint32_t r = VK_COMPONENT_SWIZZLE_IDENTITY;
        std::uint32_t g = VK_COMPONENT_SWIZZLE_IDENTITY;
        std::uint32_t b = VK_COMPONENT_SWIZZLE_IDENTITY;
        std::uint32_t a = VK_COMPONENT_SWIZZLE_IDENTITY;
    };

    struct VkImageSubresourceRange
    {
        std::uint32_t aspectMask = 0;
        std::uint32_t baseMipLevel = 0;
        std::uint32_t levelCount = 0;
        std::uint32_t baseArrayLayer = 0;
        std::uint32_t layerCount = 0;
    };

    struct VkImageSubresourceLayers
    {
        std::uint32_t aspectMask = 0;
        std::uint32_t mipLevel = 0;
        std::uint32_t baseArrayLayer = 0;
        std::uint32_t layerCount = 1;
    };

    struct VkImageViewCreateInfo
    {
        std::uint32_t sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        const void* pNext = nullptr;
        std::uint32_t flags = 0;
        void* image = nullptr;
        std::uint32_t viewType = VK_IMAGE_VIEW_TYPE_2D;
        std::uint32_t format = 0;
        VkComponentMapping components;
        VkImageSubresourceRange subresourceRange;
    };

    struct VkAttachmentDescription
    {
        std::uint32_t flags = 0;
        std::uint32_t format = 0;
        std::uint32_t samples = VK_SAMPLE_COUNT_1_BIT;
        std::uint32_t loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        std::uint32_t storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        std::uint32_t stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        std::uint32_t stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        std::uint32_t initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        std::uint32_t finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    };

    struct VkAttachmentReference
    {
        std::uint32_t attachment = 0;
        std::uint32_t layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    };

    struct VkSubpassDescription
    {
        std::uint32_t flags = 0;
        std::uint32_t pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        std::uint32_t inputAttachmentCount = 0;
        const void* pInputAttachments = nullptr;
        std::uint32_t colorAttachmentCount = 0;
        const VkAttachmentReference* pColorAttachments = nullptr;
        const void* pResolveAttachments = nullptr;
        const void* pDepthStencilAttachment = nullptr;
        std::uint32_t preserveAttachmentCount = 0;
        const std::uint32_t* pPreserveAttachments = nullptr;
    };

    struct VkRenderPassCreateInfo
    {
        std::uint32_t sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        const void* pNext = nullptr;
        std::uint32_t flags = 0;
        std::uint32_t attachmentCount = 0;
        const VkAttachmentDescription* pAttachments = nullptr;
        std::uint32_t subpassCount = 0;
        const VkSubpassDescription* pSubpasses = nullptr;
        std::uint32_t dependencyCount = 0;
        const void* pDependencies = nullptr;
    };

    struct VkFramebufferCreateInfo
    {
        std::uint32_t sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        const void* pNext = nullptr;
        std::uint32_t flags = 0;
        void* renderPass = nullptr;
        std::uint32_t attachmentCount = 0;
        void* const* pAttachments = nullptr;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t layers = 1;
    };

    struct VkCommandPoolCreateInfo
    {
        std::uint32_t sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        const void* pNext = nullptr;
        std::uint32_t flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        std::uint32_t queueFamilyIndex = 0;
    };

    struct VkCommandBufferAllocateInfo
    {
        std::uint32_t sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        const void* pNext = nullptr;
        void* commandPool = nullptr;
        std::uint32_t level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        std::uint32_t commandBufferCount = 0;
    };

    struct VkCommandBufferBeginInfo
    {
        std::uint32_t sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        const void* pNext = nullptr;
        std::uint32_t flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        const void* pInheritanceInfo = nullptr;
    };

    union VkClearColorValue
    {
        float float32[4];
        std::int32_t int32[4];
        std::uint32_t uint32[4];
    };

    union VkClearValue
    {
        VkClearColorValue color;
    };

    struct VkRenderPassBeginInfo
    {
        std::uint32_t sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        const void* pNext = nullptr;
        void* renderPass = nullptr;
        void* framebuffer = nullptr;
        VkRect2D renderArea;
        std::uint32_t clearValueCount = 0;
        const VkClearValue* pClearValues = nullptr;
    };

    struct VkClearAttachment
    {
        std::uint32_t aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        std::uint32_t colorAttachment = 0;
        VkClearValue clearValue;
    };

    struct VkClearRect
    {
        VkRect2D rect;
        std::uint32_t baseArrayLayer = 0;
        std::uint32_t layerCount = 1;
    };

    struct VkSemaphoreCreateInfo
    {
        std::uint32_t sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        const void* pNext = nullptr;
        std::uint32_t flags = 0;
    };

    constexpr std::uint32_t VK_STRUCTURE_TYPE_FENCE_CREATE_INFO = 8;
    constexpr std::uint32_t VK_FENCE_CREATE_SIGNALED_BIT = 1;

    struct VkFenceCreateInfo
    {
        std::uint32_t sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        const void* pNext = nullptr;
        std::uint32_t flags = 0;
    };

    struct VkMemoryRequirements
    {
        std::uint64_t size = 0;
        std::uint64_t alignment = 0;
        std::uint32_t memoryTypeBits = 0;
    };

    struct VkMemoryAllocateInfo
    {
        std::uint32_t sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        const void* pNext = nullptr;
        std::uint64_t allocationSize = 0;
        std::uint32_t memoryTypeIndex = 0;
    };

    struct VkBufferCreateInfo
    {
        std::uint32_t sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        const void* pNext = nullptr;
        std::uint32_t flags = 0;
        std::uint64_t size = 0;
        std::uint32_t usage = 0;
        std::uint32_t sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        std::uint32_t queueFamilyIndexCount = 0;
        const std::uint32_t* pQueueFamilyIndices = nullptr;
    };

    struct VkImageCreateInfo
    {
        std::uint32_t sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        const void* pNext = nullptr;
        std::uint32_t flags = 0;
        std::uint32_t imageType = VK_IMAGE_TYPE_2D;
        std::uint32_t format = VK_FORMAT_R8G8B8A8_UNORM;
        VkExtent3D extent;
        std::uint32_t mipLevels = 1;
        std::uint32_t arrayLayers = 1;
        std::uint32_t samples = VK_SAMPLE_COUNT_1_BIT;
        std::uint32_t tiling = VK_IMAGE_TILING_OPTIMAL;
        std::uint32_t usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        std::uint32_t sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        std::uint32_t queueFamilyIndexCount = 0;
        const std::uint32_t* pQueueFamilyIndices = nullptr;
        std::uint32_t initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    };

    struct VkBufferImageCopy
    {
        std::uint64_t bufferOffset = 0;
        std::uint32_t bufferRowLength = 0;
        std::uint32_t bufferImageHeight = 0;
        VkImageSubresourceLayers imageSubresource;
        VkOffset3D imageOffset;
        VkExtent3D imageExtent;
    };

    struct VkImageMemoryBarrier
    {
        std::uint32_t sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        const void* pNext = nullptr;
        std::uint32_t srcAccessMask = 0;
        std::uint32_t dstAccessMask = 0;
        std::uint32_t oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        std::uint32_t newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        std::uint32_t srcQueueFamilyIndex = 0xFFFFFFFFu;
        std::uint32_t dstQueueFamilyIndex = 0xFFFFFFFFu;
        void* image = nullptr;
        VkImageSubresourceRange subresourceRange;
    };

    struct VkSamplerCreateInfo
    {
        std::uint32_t sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        const void* pNext = nullptr;
        std::uint32_t flags = 0;
        std::uint32_t magFilter = VK_FILTER_LINEAR;
        std::uint32_t minFilter = VK_FILTER_LINEAR;
        std::uint32_t mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        std::uint32_t addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        std::uint32_t addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        std::uint32_t addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        float mipLodBias = 0.0f;
        std::uint32_t anisotropyEnable = 0;
        float maxAnisotropy = 1.0f;
        std::uint32_t compareEnable = 0;
        std::uint32_t compareOp = VK_COMPARE_OP_ALWAYS;
        float minLod = 0.0f;
        float maxLod = 0.0f;
        std::uint32_t borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
        std::uint32_t unnormalizedCoordinates = 0;
    };

    struct VkShaderModuleCreateInfo
    {
        std::uint32_t sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        const void* pNext = nullptr;
        std::uint32_t flags = 0;
        std::size_t codeSize = 0;
        const std::uint32_t* pCode = nullptr;
    };

    struct VkDescriptorSetLayoutBinding
    {
        std::uint32_t binding = 0;
        std::uint32_t descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        std::uint32_t descriptorCount = 1;
        std::uint32_t stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        const void* pImmutableSamplers = nullptr;
    };

    struct VkDescriptorSetLayoutCreateInfo
    {
        std::uint32_t sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        const void* pNext = nullptr;
        std::uint32_t flags = 0;
        std::uint32_t bindingCount = 0;
        const VkDescriptorSetLayoutBinding* pBindings = nullptr;
    };

    struct VkDescriptorPoolSize
    {
        std::uint32_t type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        std::uint32_t descriptorCount = 1;
    };

    struct VkDescriptorPoolCreateInfo
    {
        std::uint32_t sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        const void* pNext = nullptr;
        std::uint32_t flags = 0;
        std::uint32_t maxSets = 1;
        std::uint32_t poolSizeCount = 0;
        const VkDescriptorPoolSize* pPoolSizes = nullptr;
    };

    struct VkDescriptorSetAllocateInfo
    {
        std::uint32_t sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        const void* pNext = nullptr;
        void* descriptorPool = nullptr;
        std::uint32_t descriptorSetCount = 0;
        void* const* pSetLayouts = nullptr;
    };

    struct VkDescriptorImageInfo
    {
        void* sampler = nullptr;
        void* imageView = nullptr;
        std::uint32_t imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    };

    struct VkWriteDescriptorSet
    {
        std::uint32_t sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        const void* pNext = nullptr;
        void* dstSet = nullptr;
        std::uint32_t dstBinding = 0;
        std::uint32_t dstArrayElement = 0;
        std::uint32_t descriptorCount = 0;
        std::uint32_t descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        const VkDescriptorImageInfo* pImageInfo = nullptr;
        const void* pBufferInfo = nullptr;
        const void* pTexelBufferView = nullptr;
    };

    struct VkPushConstantRange
    {
        std::uint32_t stageFlags = 0;
        std::uint32_t offset = 0;
        std::uint32_t size = 0;
    };

    struct VkPipelineLayoutCreateInfo
    {
        std::uint32_t sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        const void* pNext = nullptr;
        std::uint32_t flags = 0;
        std::uint32_t setLayoutCount = 0;
        void* const* pSetLayouts = nullptr;
        std::uint32_t pushConstantRangeCount = 0;
        const VkPushConstantRange* pPushConstantRanges = nullptr;
    };

    struct VkPipelineShaderStageCreateInfo
    {
        std::uint32_t sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        const void* pNext = nullptr;
        std::uint32_t flags = 0;
        std::uint32_t stage = 0;
        void* module = nullptr;
        const char* pName = "main";
        const void* pSpecializationInfo = nullptr;
    };

    struct VkPipelineVertexInputStateCreateInfo
    {
        std::uint32_t sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        const void* pNext = nullptr;
        std::uint32_t flags = 0;
        std::uint32_t vertexBindingDescriptionCount = 0;
        const void* pVertexBindingDescriptions = nullptr;
        std::uint32_t vertexAttributeDescriptionCount = 0;
        const void* pVertexAttributeDescriptions = nullptr;
    };

    struct VkPipelineInputAssemblyStateCreateInfo
    {
        std::uint32_t sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        const void* pNext = nullptr;
        std::uint32_t flags = 0;
        std::uint32_t topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        std::uint32_t primitiveRestartEnable = 0;
    };

    struct VkViewport
    {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        float minDepth = 0.0f;
        float maxDepth = 1.0f;
    };

    struct VkPipelineViewportStateCreateInfo
    {
        std::uint32_t sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        const void* pNext = nullptr;
        std::uint32_t flags = 0;
        std::uint32_t viewportCount = 0;
        const VkViewport* pViewports = nullptr;
        std::uint32_t scissorCount = 0;
        const VkRect2D* pScissors = nullptr;
    };

    struct VkPipelineRasterizationStateCreateInfo
    {
        std::uint32_t sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        const void* pNext = nullptr;
        std::uint32_t flags = 0;
        std::uint32_t depthClampEnable = 0;
        std::uint32_t rasterizerDiscardEnable = 0;
        std::uint32_t polygonMode = VK_POLYGON_MODE_FILL;
        std::uint32_t cullMode = VK_CULL_MODE_NONE;
        std::uint32_t frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        std::uint32_t depthBiasEnable = 0;
        float depthBiasConstantFactor = 0.0f;
        float depthBiasClamp = 0.0f;
        float depthBiasSlopeFactor = 0.0f;
        float lineWidth = 1.0f;
    };

    struct VkPipelineMultisampleStateCreateInfo
    {
        std::uint32_t sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        const void* pNext = nullptr;
        std::uint32_t flags = 0;
        std::uint32_t rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        std::uint32_t sampleShadingEnable = 0;
        float minSampleShading = 0.0f;
        const void* pSampleMask = nullptr;
        std::uint32_t alphaToCoverageEnable = 0;
        std::uint32_t alphaToOneEnable = 0;
    };

    struct VkPipelineColorBlendAttachmentState
    {
        std::uint32_t blendEnable = 0;
        std::uint32_t srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        std::uint32_t dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        std::uint32_t colorBlendOp = VK_BLEND_OP_ADD;
        std::uint32_t srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        std::uint32_t dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        std::uint32_t alphaBlendOp = VK_BLEND_OP_ADD;
        std::uint32_t colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    };

    struct VkPipelineColorBlendStateCreateInfo
    {
        std::uint32_t sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        const void* pNext = nullptr;
        std::uint32_t flags = 0;
        std::uint32_t logicOpEnable = 0;
        std::uint32_t logicOp = VK_LOGIC_OP_COPY;
        std::uint32_t attachmentCount = 0;
        const VkPipelineColorBlendAttachmentState* pAttachments = nullptr;
        float blendConstants[4] = {};
    };

    struct VkGraphicsPipelineCreateInfo
    {
        std::uint32_t sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        const void* pNext = nullptr;
        std::uint32_t flags = 0;
        std::uint32_t stageCount = 0;
        const VkPipelineShaderStageCreateInfo* pStages = nullptr;
        const VkPipelineVertexInputStateCreateInfo* pVertexInputState = nullptr;
        const VkPipelineInputAssemblyStateCreateInfo* pInputAssemblyState = nullptr;
        const void* pTessellationState = nullptr;
        const VkPipelineViewportStateCreateInfo* pViewportState = nullptr;
        const VkPipelineRasterizationStateCreateInfo* pRasterizationState = nullptr;
        const VkPipelineMultisampleStateCreateInfo* pMultisampleState = nullptr;
        const void* pDepthStencilState = nullptr;
        const VkPipelineColorBlendStateCreateInfo* pColorBlendState = nullptr;
        const void* pDynamicState = nullptr;
        void* layout = nullptr;
        void* renderPass = nullptr;
        std::uint32_t subpass = 0;
        void* basePipelineHandle = nullptr;
        std::int32_t basePipelineIndex = -1;
    };

    struct VkSubmitInfo
    {
        std::uint32_t sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        const void* pNext = nullptr;
        std::uint32_t waitSemaphoreCount = 0;
        const void* const* pWaitSemaphores = nullptr;
        const std::uint32_t* pWaitDstStageMask = nullptr;
        std::uint32_t commandBufferCount = 0;
        const void* const* pCommandBuffers = nullptr;
        std::uint32_t signalSemaphoreCount = 0;
        const void* const* pSignalSemaphores = nullptr;
    };

    using QueuePresentFn = std::int32_t(__fastcall*)(void* queue, const void* presentInfo);
    using CreateSwapchainFn = std::int32_t(__fastcall*)(void* device, const VkSwapchainCreateInfoKHR* createInfo, const void* allocator, void** swapchain);
    using GetSwapchainImagesFn = std::int32_t(__fastcall*)(void* device, void* swapchain, std::uint32_t* count, void* images);
    using GetDeviceProcAddrFn = void* (__fastcall*)(void* device, const char* name);
    using CreateImageViewFn = std::int32_t(__fastcall*)(void* device, const VkImageViewCreateInfo* createInfo, const void* allocator, void** imageView);
    using DestroyImageViewFn = void(__fastcall*)(void* device, void* imageView, const void* allocator);
    using CreateRenderPassFn = std::int32_t(__fastcall*)(void* device, const VkRenderPassCreateInfo* createInfo, const void* allocator, void** renderPass);
    using DestroyRenderPassFn = void(__fastcall*)(void* device, void* renderPass, const void* allocator);
    using CreateFramebufferFn = std::int32_t(__fastcall*)(void* device, const VkFramebufferCreateInfo* createInfo, const void* allocator, void** framebuffer);
    using DestroyFramebufferFn = void(__fastcall*)(void* device, void* framebuffer, const void* allocator);
    using CreateCommandPoolFn = std::int32_t(__fastcall*)(void* device, const VkCommandPoolCreateInfo* createInfo, const void* allocator, void** commandPool);
    using DestroyCommandPoolFn = void(__fastcall*)(void* device, void* commandPool, const void* allocator);
    using AllocateCommandBuffersFn = std::int32_t(__fastcall*)(void* device, const VkCommandBufferAllocateInfo* allocateInfo, void** commandBuffers);
    using ResetCommandBufferFn = std::int32_t(__fastcall*)(void* commandBuffer, std::uint32_t flags);
    using BeginCommandBufferFn = std::int32_t(__fastcall*)(void* commandBuffer, const VkCommandBufferBeginInfo* beginInfo);
    using EndCommandBufferFn = std::int32_t(__fastcall*)(void* commandBuffer);
    using CmdBeginRenderPassFn = void(__fastcall*)(void* commandBuffer, const VkRenderPassBeginInfo* beginInfo, std::uint32_t contents);
    using CmdEndRenderPassFn = void(__fastcall*)(void* commandBuffer);
    using CmdClearAttachmentsFn = void(__fastcall*)(void* commandBuffer, std::uint32_t attachmentCount, const VkClearAttachment* attachments, std::uint32_t rectCount, const VkClearRect* rects);
    using CreateBufferFn = std::int32_t(__fastcall*)(void* device, const VkBufferCreateInfo* createInfo, const void* allocator, void** buffer);
    using DestroyBufferFn = void(__fastcall*)(void* device, void* buffer, const void* allocator);
    using GetBufferMemoryRequirementsFn = void(__fastcall*)(void* device, void* buffer, VkMemoryRequirements* memoryRequirements);
    using CreateImageFn = std::int32_t(__fastcall*)(void* device, const VkImageCreateInfo* createInfo, const void* allocator, void** image);
    using DestroyImageFn = void(__fastcall*)(void* device, void* image, const void* allocator);
    using GetImageMemoryRequirementsFn = void(__fastcall*)(void* device, void* image, VkMemoryRequirements* memoryRequirements);
    using AllocateMemoryFn = std::int32_t(__fastcall*)(void* device, const VkMemoryAllocateInfo* allocateInfo, const void* allocator, void** memory);
    using FreeMemoryFn = void(__fastcall*)(void* device, void* memory, const void* allocator);
    using BindBufferMemoryFn = std::int32_t(__fastcall*)(void* device, void* buffer, void* memory, std::uint64_t memoryOffset);
    using BindImageMemoryFn = std::int32_t(__fastcall*)(void* device, void* image, void* memory, std::uint64_t memoryOffset);
    using MapMemoryFn = std::int32_t(__fastcall*)(void* device, void* memory, std::uint64_t offset, std::uint64_t size, std::uint32_t flags, void** data);
    using UnmapMemoryFn = void(__fastcall*)(void* device, void* memory);
    using CreateSamplerFn = std::int32_t(__fastcall*)(void* device, const VkSamplerCreateInfo* createInfo, const void* allocator, void** sampler);
    using DestroySamplerFn = void(__fastcall*)(void* device, void* sampler, const void* allocator);
    using CreateShaderModuleFn = std::int32_t(__fastcall*)(void* device, const VkShaderModuleCreateInfo* createInfo, const void* allocator, void** shaderModule);
    using DestroyShaderModuleFn = void(__fastcall*)(void* device, void* shaderModule, const void* allocator);
    using CreateDescriptorSetLayoutFn = std::int32_t(__fastcall*)(void* device, const VkDescriptorSetLayoutCreateInfo* createInfo, const void* allocator, void** setLayout);
    using DestroyDescriptorSetLayoutFn = void(__fastcall*)(void* device, void* setLayout, const void* allocator);
    using CreateDescriptorPoolFn = std::int32_t(__fastcall*)(void* device, const VkDescriptorPoolCreateInfo* createInfo, const void* allocator, void** descriptorPool);
    using DestroyDescriptorPoolFn = void(__fastcall*)(void* device, void* descriptorPool, const void* allocator);
    using AllocateDescriptorSetsFn = std::int32_t(__fastcall*)(void* device, const VkDescriptorSetAllocateInfo* allocateInfo, void** descriptorSets);
    using UpdateDescriptorSetsFn = void(__fastcall*)(void* device, std::uint32_t descriptorWriteCount, const VkWriteDescriptorSet* descriptorWrites, std::uint32_t descriptorCopyCount, const void* descriptorCopies);
    using CreatePipelineLayoutFn = std::int32_t(__fastcall*)(void* device, const VkPipelineLayoutCreateInfo* createInfo, const void* allocator, void** pipelineLayout);
    using DestroyPipelineLayoutFn = void(__fastcall*)(void* device, void* pipelineLayout, const void* allocator);
    using CreateGraphicsPipelinesFn = std::int32_t(__fastcall*)(void* device, void* pipelineCache, std::uint32_t createInfoCount, const VkGraphicsPipelineCreateInfo* createInfos, const void* allocator, void** pipelines);
    using DestroyPipelineFn = void(__fastcall*)(void* device, void* pipeline, const void* allocator);
    using CmdPipelineBarrierFn = void(__fastcall*)(void* commandBuffer, std::uint32_t srcStageMask, std::uint32_t dstStageMask, std::uint32_t dependencyFlags, std::uint32_t memoryBarrierCount, const void* memoryBarriers, std::uint32_t bufferMemoryBarrierCount, const void* bufferMemoryBarriers, std::uint32_t imageMemoryBarrierCount, const VkImageMemoryBarrier* imageMemoryBarriers);
    using CmdCopyBufferToImageFn = void(__fastcall*)(void* commandBuffer, void* srcBuffer, void* dstImage, std::uint32_t dstImageLayout, std::uint32_t regionCount, const VkBufferImageCopy* regions);
    using CmdBindPipelineFn = void(__fastcall*)(void* commandBuffer, std::uint32_t pipelineBindPoint, void* pipeline);
    using CmdBindDescriptorSetsFn = void(__fastcall*)(void* commandBuffer, std::uint32_t pipelineBindPoint, void* layout, std::uint32_t firstSet, std::uint32_t descriptorSetCount, void* const* descriptorSets, std::uint32_t dynamicOffsetCount, const std::uint32_t* dynamicOffsets);
    using CmdPushConstantsFn = void(__fastcall*)(void* commandBuffer, void* layout, std::uint32_t stageFlags, std::uint32_t offset, std::uint32_t size, const void* values);
    using CmdDrawFn = void(__fastcall*)(void* commandBuffer, std::uint32_t vertexCount, std::uint32_t instanceCount, std::uint32_t firstVertex, std::uint32_t firstInstance);
    using CreateSemaphoreFn = std::int32_t(__fastcall*)(void* device, const VkSemaphoreCreateInfo* createInfo, const void* allocator, void** semaphore);
    using CreateFenceFn = std::int32_t(__fastcall*)(void* device, const VkFenceCreateInfo* createInfo, const void* allocator, void** fence);
    using DestroyFenceFn = void(__fastcall*)(void* device, void* fence, const void* allocator);
    using WaitForFencesFn = std::int32_t(__fastcall*)(void* device, std::uint32_t fenceCount, const void* const* fences, std::uint32_t waitAll, std::uint64_t timeout);
    using ResetFencesFn = std::int32_t(__fastcall*)(void* device, std::uint32_t fenceCount, const void* const* fences);
    using DestroySemaphoreFn = void(__fastcall*)(void* device, void* semaphore, const void* allocator);
    using QueueSubmitFn = std::int32_t(__fastcall*)(void* queue, std::uint32_t submitCount, const VkSubmitInfo* submits, void* fence);
    using DeviceWaitIdleFn = std::int32_t(__fastcall*)(void* device);

    struct UiRenderSnapshot
    {
        uintptr_t source = 0;
        uintptr_t state = 0;
        std::int32_t sourceWidth = 0;
        std::int32_t sourceHeight = 0;
        std::int32_t stateWidth = 0;
        std::int32_t stateHeight = 0;
    };

    struct SwapchainRuntimeInfo
    {
        uintptr_t handle = 0;
        uintptr_t device = 0;
        std::uint32_t format = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t minImageCount = 0;
        std::uint32_t imageCount = 0;
        std::vector<uintptr_t> images;
    };

    struct VulkanRendererFns
    {
        GetSwapchainImagesFn getSwapchainImages = nullptr;
        CreateImageViewFn createImageView = nullptr;
        DestroyImageViewFn destroyImageView = nullptr;
        CreateRenderPassFn createRenderPass = nullptr;
        DestroyRenderPassFn destroyRenderPass = nullptr;
        CreateFramebufferFn createFramebuffer = nullptr;
        DestroyFramebufferFn destroyFramebuffer = nullptr;
        CreateCommandPoolFn createCommandPool = nullptr;
        DestroyCommandPoolFn destroyCommandPool = nullptr;
        AllocateCommandBuffersFn allocateCommandBuffers = nullptr;
        ResetCommandBufferFn resetCommandBuffer = nullptr;
        BeginCommandBufferFn beginCommandBuffer = nullptr;
        EndCommandBufferFn endCommandBuffer = nullptr;
        CmdBeginRenderPassFn cmdBeginRenderPass = nullptr;
        CmdEndRenderPassFn cmdEndRenderPass = nullptr;
        CmdClearAttachmentsFn cmdClearAttachments = nullptr;
        CreateBufferFn createBuffer = nullptr;
        DestroyBufferFn destroyBuffer = nullptr;
        GetBufferMemoryRequirementsFn getBufferMemoryRequirements = nullptr;
        CreateImageFn createImage = nullptr;
        DestroyImageFn destroyImage = nullptr;
        GetImageMemoryRequirementsFn getImageMemoryRequirements = nullptr;
        AllocateMemoryFn allocateMemory = nullptr;
        FreeMemoryFn freeMemory = nullptr;
        BindBufferMemoryFn bindBufferMemory = nullptr;
        BindImageMemoryFn bindImageMemory = nullptr;
        MapMemoryFn mapMemory = nullptr;
        UnmapMemoryFn unmapMemory = nullptr;
        CreateSamplerFn createSampler = nullptr;
        DestroySamplerFn destroySampler = nullptr;
        CreateShaderModuleFn createShaderModule = nullptr;
        DestroyShaderModuleFn destroyShaderModule = nullptr;
        CreateDescriptorSetLayoutFn createDescriptorSetLayout = nullptr;
        DestroyDescriptorSetLayoutFn destroyDescriptorSetLayout = nullptr;
        CreateDescriptorPoolFn createDescriptorPool = nullptr;
        DestroyDescriptorPoolFn destroyDescriptorPool = nullptr;
        AllocateDescriptorSetsFn allocateDescriptorSets = nullptr;
        UpdateDescriptorSetsFn updateDescriptorSets = nullptr;
        CreatePipelineLayoutFn createPipelineLayout = nullptr;
        DestroyPipelineLayoutFn destroyPipelineLayout = nullptr;
        CreateGraphicsPipelinesFn createGraphicsPipelines = nullptr;
        DestroyPipelineFn destroyPipeline = nullptr;
        CmdPipelineBarrierFn cmdPipelineBarrier = nullptr;
        CmdCopyBufferToImageFn cmdCopyBufferToImage = nullptr;
        CmdBindPipelineFn cmdBindPipeline = nullptr;
        CmdBindDescriptorSetsFn cmdBindDescriptorSets = nullptr;
        CmdPushConstantsFn cmdPushConstants = nullptr;
        CmdDrawFn cmdDraw = nullptr;
        CreateSemaphoreFn createSemaphore = nullptr;
        DestroySemaphoreFn destroySemaphore = nullptr;
        CreateFenceFn createFence = nullptr;
        DestroyFenceFn destroyFence = nullptr;
        WaitForFencesFn waitForFences = nullptr;
        ResetFencesFn resetFences = nullptr;
        QueueSubmitFn queueSubmit = nullptr;
        DeviceWaitIdleFn deviceWaitIdle = nullptr;

        bool Ready() const
        {
            return getSwapchainImages != nullptr &&
                createImageView != nullptr &&
                destroyImageView != nullptr &&
                createRenderPass != nullptr &&
                destroyRenderPass != nullptr &&
                createFramebuffer != nullptr &&
                destroyFramebuffer != nullptr &&
                createCommandPool != nullptr &&
                destroyCommandPool != nullptr &&
                allocateCommandBuffers != nullptr &&
                resetCommandBuffer != nullptr &&
                beginCommandBuffer != nullptr &&
                endCommandBuffer != nullptr &&
                cmdBeginRenderPass != nullptr &&
                cmdEndRenderPass != nullptr &&
                cmdClearAttachments != nullptr &&
                createSemaphore != nullptr &&
                destroySemaphore != nullptr &&
                createFence != nullptr &&
                destroyFence != nullptr &&
                waitForFences != nullptr &&
                resetFences != nullptr &&
                queueSubmit != nullptr &&
                deviceWaitIdle != nullptr;
        }

        bool TextureReady() const
        {
            return createBuffer != nullptr &&
                destroyBuffer != nullptr &&
                getBufferMemoryRequirements != nullptr &&
                createImage != nullptr &&
                destroyImage != nullptr &&
                getImageMemoryRequirements != nullptr &&
                allocateMemory != nullptr &&
                freeMemory != nullptr &&
                bindBufferMemory != nullptr &&
                bindImageMemory != nullptr &&
                mapMemory != nullptr &&
                unmapMemory != nullptr &&
                createSampler != nullptr &&
                destroySampler != nullptr &&
                createShaderModule != nullptr &&
                destroyShaderModule != nullptr &&
                createDescriptorSetLayout != nullptr &&
                destroyDescriptorSetLayout != nullptr &&
                createDescriptorPool != nullptr &&
                destroyDescriptorPool != nullptr &&
                allocateDescriptorSets != nullptr &&
                updateDescriptorSets != nullptr &&
                createPipelineLayout != nullptr &&
                destroyPipelineLayout != nullptr &&
                createGraphicsPipelines != nullptr &&
                destroyPipeline != nullptr &&
                cmdPipelineBarrier != nullptr &&
                cmdCopyBufferToImage != nullptr &&
                cmdBindPipeline != nullptr &&
                cmdBindDescriptorSets != nullptr &&
                cmdPushConstants != nullptr &&
                cmdDraw != nullptr;
        }
    };

    // One mipmapped RGBA texture + textured-quad pipeline (map, sprite atlas).
    struct GpuTexturePipeline
    {
        uintptr_t image = 0;
        uintptr_t memory = 0;
        uintptr_t imageView = 0;
        uintptr_t sampler = 0;
        uintptr_t stagingBuffer = 0;
        uintptr_t stagingMemory = 0;
        uintptr_t descriptorSetLayout = 0;
        uintptr_t descriptorPool = 0;
        uintptr_t descriptorSet = 0;
        uintptr_t pipelineLayout = 0;
        uintptr_t pipeline = 0;
        std::uint32_t size = 0;
        std::uint32_t mipLevels = 0;
        std::uint64_t uploadBytes = 0;
        std::vector<VkBufferImageCopy> uploadRegions;
        std::uint32_t uploadImageIndex = 0;
        bool stagingReleasePending = false;
        bool uploadPending = false;
        bool ready = false;
        bool attempted = false;
        // Re-uploadable texture (fog of war): the staging buffer stays mapped and is
        // refilled whenever the content changes. stagingBusy is set from the moment an
        // upload is recorded until that command buffer's fence has been waited on.
        bool persistentStaging = false;
        bool stagingBusy = false;
        bool uploadedOnce = false;
        std::uint8_t* stagingMapped = nullptr;
        std::uint64_t contentVersion = 0;
    };

    // A pipeline replaced while command buffers that reference it may still be in
    // flight. It is destroyed once every swapchain image has been re-recorded (each
    // re-record waits on that image's fence first), without stalling the device.
    struct RetiredGpuPipeline
    {
        GpuTexturePipeline gp;
        std::uint32_t seenImageMask = 0;
        std::uint32_t records = 0;
    };

    struct GpuSpriteRect
    {
        std::uint32_t key = 0;
        float u0 = 0.0f;
        float v0 = 0.0f;
        float u1 = 0.0f;
        float v1 = 0.0f;
        float aspect = 1.0f;
    };

    struct VulkanMinimapRenderer
    {
        bool ready = false;
        uintptr_t device = 0;
        uintptr_t swapchain = 0;
        std::uint32_t format = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t queueFamilyIndex = 0;
        std::vector<uintptr_t> images;
        std::vector<uintptr_t> imageViews;
        std::vector<uintptr_t> framebuffers;
        std::vector<uintptr_t> commandBuffers;
        std::vector<uintptr_t> renderCompleteSemaphores;
        std::vector<uintptr_t> commandFences;
        uintptr_t renderPass = 0;
        uintptr_t commandPool = 0;
        uintptr_t frameTextureImage = 0;
        uintptr_t frameTextureMemory = 0;
        uintptr_t frameTextureImageView = 0;
        uintptr_t frameSampler = 0;
        uintptr_t frameStagingBuffer = 0;
        uintptr_t frameStagingMemory = 0;
        uintptr_t frameDescriptorSetLayout = 0;
        uintptr_t frameDescriptorPool = 0;
        uintptr_t frameDescriptorSet = 0;
        uintptr_t framePipelineLayout = 0;
        uintptr_t framePipeline = 0;
        std::uint32_t frameTextureWidth = 0;
        std::uint32_t frameTextureHeight = 0;
        bool frameTextureUploadPending = false;
        bool frameTextureReady = false;
        GpuTexturePipeline mapGpu;
        GpuTexturePipeline spriteGpu;
        GpuTexturePipeline fogGpu;
        std::vector<GpuSpriteRect> spriteRects;
        std::vector<RetiredGpuPipeline> retiredPipelines;
        // Redundant-bind filter, valid only inside one command-buffer recording.
        uintptr_t boundPipeline = 0;
        uintptr_t boundDescriptorSet = 0;
        VulkanRendererFns fns;
    };

    struct RealMapTexture
    {
        bool attempted = false;
        bool loaded = false;
        int width = 0;
        int height = 0;
        std::string path;
        std::vector<std::uint8_t> rgba;
    };

    struct StaticPoi
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        std::uint32_t kind = 0;
    };

    struct StaticPoiCatalog
    {
        bool attempted = false;
        bool loaded = false;
        std::string path;
        std::vector<StaticPoi> pois;
    };

    struct FogOfWarMask
    {
        bool attempted = false;
        bool loaded = false;
        int width = 0;
        int height = 0;
        std::string path;
        std::vector<std::uint8_t> values;
    };

    struct MinimapIcon
    {
        std::uint32_t key = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::vector<std::uint8_t> rgba;
    };

    struct MinimapIconAtlas
    {
        bool attempted = false;
        bool loaded = false;
        std::string path;
        std::vector<MinimapIcon> icons;
    };

    struct MinimapFrameRun
    {
        std::uint16_t x = 0;
        std::uint16_t y = 0;
        std::uint16_t width = 0;
    };

    struct MinimapFrameColorGroup
    {
        std::uint8_t red = 0;
        std::uint8_t green = 0;
        std::uint8_t blue = 0;
        std::vector<MinimapFrameRun> runs;
    };

    struct MinimapFrameAsset
    {
        bool attempted = false;
        bool loaded = false;
        int width = 0;
        int height = 0;
        std::string path;
        std::vector<std::uint8_t> rgba;
        std::vector<MinimapFrameColorGroup> groups;
    };

    ModMetaData g_metaData = {
        "minimap_mod",
        "Internal minimap data bridge for Enshrouded. No external overlay window.",
        "0.4.46-fix17",
        "OpenAI + xoker",
        "0.0.3",
        true,
        false
    };

    ModContext* g_modContext = nullptr;
    uintptr_t g_exeBase = 0;
    std::size_t g_exeImageSize = 0;
    IterInitFn g_iterInit = nullptr;
    IterNextFn g_iterNext = nullptr;
    Mem::Detour* g_localPlayerUiRenderSetupHook = nullptr;
    Mem::Detour* g_playerWaypointsUiHook = nullptr;
    Mem::Detour* g_mapMarkerVisibilityHook = nullptr;
    Mem::Detour* g_fogOfWarHook = nullptr;
    Mem::Detour* g_renderPresentFrameHook = nullptr;
    Mem::Detour* g_vulkanDeviceTableInitHook = nullptr;

    std::mutex g_waypointMutex;
    std::vector<CapturedWaypoint> g_waypoints;
    std::mutex g_nearbyMarkerMutex;
    std::vector<CapturedNearbyMarker> g_nearbyMarkers;
    std::mutex g_visibleMapMarkerMutex;
    std::vector<CapturedMapMarkerVisibility> g_visibleMapMarkers;
    std::mutex g_playerPositionMutex;
    CapturedPlayerPosition g_playerPosition;
    DWORD g_lastSummaryTick = 0;
    DWORD g_lastNearbySummaryTick = 0;
    DWORD g_lastVisibleMapMarkerSummaryTick = 0;
    DWORD g_lastVisibleMapMarkerPruneTick = 0;
    DWORD g_lastVisibleMapMarkerFaultTick = 0;
    DWORD g_lastPlayerPositionSummaryTick = 0;
    DWORD g_lastRenderFrameSummaryTick = 0;
    DWORD g_lastUiRenderSummaryTick = 0;
    DWORD g_lastVulkanSummaryTick = 0;
    DWORD g_lastVulkanHookSummaryTick = 0;
    DWORD g_lastVulkanPresentHookTick = 0;
    DWORD g_lastVulkanPresentInfoTick = 0;
    DWORD g_lastVulkanSwapchainHookTick = 0;
    DWORD g_lastVulkanSwapchainImagesHookTick = 0;
    DWORD g_lastVulkanScanTick = 0;
    DWORD g_lastVulkanFunctionScanTick = 0;
    DWORD g_lastVulkanRendererLogTick = 0;
    int g_vulkanScanAttempts = 0;
    bool g_vulkanFunctionOffsetsLogged = false;
    std::atomic<uintptr_t> g_vulkanDeviceTable{ 0 };
    std::atomic<uintptr_t> g_patchedVulkanDeviceTable{ 0 };
    std::atomic<uintptr_t> g_originalQueuePresent{ 0 };
    std::atomic<uintptr_t> g_originalCreateSwapchain{ 0 };
    std::atomic<uintptr_t> g_originalGetSwapchainImages{ 0 };
    std::atomic<uintptr_t> g_lastRenderContext{ 0 };
    std::atomic<uintptr_t> g_lastVulkanDevice{ 0 };
    std::atomic<uintptr_t> g_lastGraphicsContext{ 0 };
    std::atomic<uintptr_t> g_lastSwapchainState{ 0 };
    std::atomic<std::uint32_t> g_lastVulkanQueueFamilyIndex{ 0 };
    std::atomic<int> g_visibleMapMarkerFaults{ 0 };
    std::atomic<bool> g_visibleMapMarkerCaptureDisabled{ false };
    std::mutex g_vulkanHookMutex;
    std::mutex g_vulkanFunctionScanMutex;
    std::mutex g_swapchainMutex;
    std::mutex g_rendererMutex;
    std::vector<SwapchainRuntimeInfo> g_swapchains;
    VulkanMinimapRenderer g_renderer;
    std::mutex g_realMapMutex;
    RealMapTexture g_realMap;
    std::mutex g_staticPoiMutex;
    StaticPoiCatalog g_staticPois;
    std::mutex g_fogOfWarMutex;
    FogOfWarMask g_fogOfWar;
    std::mutex g_minimapIconMutex;
    MinimapIconAtlas g_minimapIcons;
    std::mutex g_minimapFrameMutex;
    MinimapFrameAsset g_minimapFrame;
    std::atomic<bool> g_worldSessionReady{ false };
    // Every background tracker loop runs only while a world is up AND the mod is on.
    bool MinimapRuntimeActive();
    std::atomic<bool> g_gameSessionOnline{ false };
    std::atomic<DWORD> g_lastWorldDataTick{ 0 };
    std::atomic<int> g_minimapZoomStep{ 0 };
    // Layout edit mode (Esc menu). See UpdateMinimapLayoutEdit.
    std::atomic<bool> g_layoutEditMode{ false };
    std::atomic<int> g_layoutOffsetX{ 0 };
    std::atomic<int> g_layoutOffsetY{ 0 };
    std::atomic<int> g_layoutRadius{ 0 };          // 0 = automatic size
    std::atomic<int> g_layoutRectCx{ 0 };
    std::atomic<int> g_layoutRectCy{ 0 };
    std::atomic<int> g_layoutRectHalf{ 0 };
    std::atomic<int> g_layoutRectRadius{ 0 };
    std::atomic<int> g_layoutRectFrameExtra{ 0 };
    std::atomic<std::uint32_t> g_layoutScreenWidth{ 0 };
    std::atomic<std::uint32_t> g_layoutScreenHeight{ 0 };
    constexpr int LAYOUT_RADIUS_MIN = 60;
    constexpr int LAYOUT_RADIUS_MAX = 320;
    constexpr int LAYOUT_HANDLE_SIZE = 14;
    // "label_font_size" in shroudtopia.json: height of the player/ping name labels.
    constexpr int MINIMAP_LABEL_SIZE_DEFAULT = 17;
    std::atomic<int> g_minimapLabelFontSize{ MINIMAP_LABEL_SIZE_DEFAULT };
    // "map_texture_size" in shroudtopia.json: the HD map (8192) is box-filtered down to
    // this size at load time. 4096 cuts VRAM for the map from ~358 MB to ~90 MB.
    constexpr int MINIMAP_MAP_TEXTURE_SIZE_DEFAULT = 8192;
    std::atomic<int> g_minimapMapTextureSize{ MINIMAP_MAP_TEXTURE_SIZE_DEFAULT };
    std::atomic<int> g_nameSpriteBuiltSize{ 0 };
    // "fog_strength" in shroudtopia.json (0..100, default 100): opacity of the slate
    // grey that covers the parts of the map you have not discovered yet. 100 hides the
    // terrain completely (like the world map); 0 turns the overlay off.
    constexpr int MINIMAP_FOG_STRENGTH_DEFAULT = 100;
    std::atomic<int> g_minimapFogStrength{ MINIMAP_FOG_STRENGTH_DEFAULT };

    // Live copy of the game's fog-of-war grid (see FogOfWarIterationRecord).
    struct FogOfWarGrid
    {
        bool valid = false;
        float sizeX = 0.0f;
        float sizeZ = 0.0f;
        std::uint32_t width = 0;     // cells
        std::uint32_t height = 0;
        std::vector<std::uint8_t> cells;   // row-major, row 0 at z = sizeZ
        std::uint64_t version = 0;
        DWORD lastCaptureTick = 0;
        uintptr_t address = 0;
    };
    std::mutex g_fogGridMutex;
    FogOfWarGrid g_fogGrid;
    std::atomic<bool> g_fogGridLogged{ false };
    std::atomic<bool> g_minimapVisible{ true };
    // The F10 master switch. Off means the mod does nothing at all: the game-thread
    // hooks return immediately, the background trackers stop, no command buffer is
    // recorded or submitted in the present hook, and every Vulkan object (the map
    // texture and the sprite atlas, together a few hundred MB of VRAM) is released.
    std::atomic<bool> g_minimapEnabled{ true };
    std::atomic<bool> g_minimapTeardownPending{ false };

    // Set by Unload: every background loop leaves, and no new one starts.
    std::atomic<bool> g_shuttingDown{ false };
    std::atomic<int> g_backgroundThreads{ 0 };

    // Counts a detached worker for the duration of its body so Unload can wait for it
    // instead of letting the DLL disappear underneath a running thread.
    struct BackgroundThreadScope
    {
        BackgroundThreadScope() { g_backgroundThreads.fetch_add(1); }
        ~BackgroundThreadScope() { g_backgroundThreads.fetch_sub(1); }
        BackgroundThreadScope(const BackgroundThreadScope&) = delete;
        BackgroundThreadScope& operator=(const BackgroundThreadScope&) = delete;
    };

    bool MinimapRuntimeActive()
    {
        return !g_shuttingDown.load() && g_worldSessionReady.load() && g_minimapEnabled.load();
    }

    // F11 (config "heading_toggle_key"): the view-direction feature. Off means no
    // heading work at all and our own marker becomes a round lime dot.
    std::atomic<bool> g_headingEnabled{ true };
    std::atomic<int> g_headingToggleKey{ VK_F11 };
    // The view direction is the direction of travel, taken from the position feed at
    // no cost. (The old camera tracker found the camera with a full scan of the game's
    // memory - ~2.4 GB, 1-2 s of one core every couple of minutes - and was removed.)

    // Drops the direction learned so far.
    void ResetHeadingState();
    std::atomic<int> g_minimapToggleKey{ VK_F10 };
    std::atomic<bool> g_debugLoggingEnabled{ false };
    std::atomic<int> g_minimapMapSampleStep{ MINIMAP_DEFAULT_MAP_SAMPLE_STEP };
    // 0..100: how far the minimap terrain is lifted toward the big map's light
    // parchment look (0 = original dark satmap). Live-tunable via "map_light".
    std::atomic<int> g_minimapMapLight{ 55 };
    // "map_renderer": "gpu" (default) draws the map as a mipmapped texture with a
    // fragment shader; "cpu" forces the old clear-rect rasterizer.
    std::atomic<bool> g_minimapMapGpuEnabled{ true };
    // "map_follow": "center" (default) keeps the player in the middle and scrolls the
    // map; "static" keeps the map still (north-up) and moves the arrow, re-centering
    // the view once the arrow gets close to the rim.
    std::atomic<bool> g_minimapStaticView{ false };
    std::atomic<int> g_minimapMaxDrawnPoints{ MINIMAP_DEFAULT_MAX_DRAWN_POINTS };
    DWORD g_lastConfigPollTick = 0;
    DWORD g_lastSessionLogPollTick = 0;
    std::string g_gameLogPath;
    std::string g_shroudtopiaConfigPath;


    std::array<std::uint8_t, LOCAL_PLAYER_UI_RENDER_SETUP_STOLEN_SIZE> g_localPlayerUiRenderSetupExpected = {
        0x48, 0x89, 0x7C, 0x24, 0x18,
        0x55,
        0x48, 0x8B, 0xEC,
        0x48, 0x83, 0xEC, 0x50
    };

    std::array<std::uint8_t, PLAYER_WAYPOINTS_UI_STOLEN_SIZE> g_playerWaypointsUiExpected = {
        0x48, 0x89, 0x5C, 0x24, 0x08,
        0x48, 0x89, 0x6C, 0x24, 0x10,
        0x48, 0x89, 0x74, 0x24, 0x18
    };

    std::array<std::uint8_t, KNOWLEDGE_QUERY_MAPMARKER_VISIBILITY_STOLEN_SIZE> g_mapMarkerVisibilityExpected = {
        0x40, 0x55,
        0x41, 0x56,
        0x48, 0x8B, 0xEC,
        0x48, 0x83, 0xEC, 0x78
    };

    std::array<std::uint8_t, KNOWLEDGE_QUERY_MAPMARKER_VISIBILITY_LOOP_STOLEN_SIZE> g_mapMarkerVisibilityLoopExpected = {
        0x48, 0x89, 0x74, 0x24, 0x68
    };

    std::array<std::uint8_t, FOG_OF_WAR_STOLEN_SIZE> g_fogOfWarExpected = {
        0x40, 0x57,                                 // push rdi
        0x48, 0x83, 0xEC, 0x60,                     // sub rsp, 60h
        0x41, 0xB8, 0x30, 0x00, 0x00, 0x00,         // mov r8d, 30h (iteration record size)
        0x48, 0x8D, 0x54, 0x24, 0x20                // lea rdx, [rsp+20h]
    };

    std::array<std::uint8_t, RENDER_PRESENT_FRAME_STOLEN_SIZE> g_renderPresentFrameExpected = {
        0x4C, 0x89, 0x4C, 0x24, 0x20,
        0x48, 0x89, 0x54, 0x24, 0x10,
        0x55,
        0x53,
        0x56,
        0x57,
        0x41, 0x54
    };

    std::array<std::uint8_t, VULKAN_DEVICE_TABLE_INIT_STOLEN_SIZE> g_vulkanDeviceTableInitExpected = {
        0x48, 0x89, 0x5C, 0x24, 0x08,
        0x48, 0x89, 0x74, 0x24, 0x10,
        0x48, 0x89, 0x7C, 0x24, 0x18
    };

    bool TryInstallVulkanTableHooks(uintptr_t table);
    void RestoreVulkanTableHooks();
    bool TryReadMinimapConfigStringFromFile(ModContext* modContext, const char* key, std::string& outValue, std::string& outSource);
    void LogRendererThrottled(const std::string& message);

    std::size_t MinValueSize(std::size_t left, std::size_t right)
    {
        return left < right ? left : right;
    }

    void Log(const std::string& message)
    {
        if (g_modContext == nullptr)
            return;

        // Shroudtopia cuts log lines at about 512 characters: split long ones at " | ".
        constexpr std::size_t LOG_CHUNK = 420;
        if (message.size() <= LOG_CHUNK)
        {
            g_modContext->Log(message.c_str());
            return;
        }

        std::size_t start = 0;
        bool first = true;
        while (start < message.size())
        {
            std::size_t end = MinValueSize(message.size(), start + LOG_CHUNK);
            if (end < message.size())
            {
                const std::size_t cut = message.rfind(" | ", end);
                if (cut != std::string::npos && cut > start)
                    end = cut;
            }
            std::string part = message.substr(start, end - start);
            if (!first)
                part = "[Minimap]   ..." + part;
            g_modContext->Log(part.c_str());
            first = false;
            start = end;
        }
    }

    std::string Hex(uintptr_t value)
    {
        std::ostringstream oss;
        oss << "0x" << std::hex << value;
        return oss.str();
    }

    template <typename T>
    T MinValue(T left, T right)
    {
        return left < right ? left : right;
    }

    template <typename T>
    T MaxValue(T left, T right)
    {
        return left > right ? left : right;
    }

    template <typename T>
    T ClampValue(T value, T low, T high)
    {
        return MaxValue(low, MinValue(value, high));
    }

    std::string NormalizeConfigValue(const std::string& value)
    {
        std::string normalized;
        normalized.reserve(value.size());
        for (char ch : value)
        {
            if (ch >= 'A' && ch <= 'Z')
                ch = static_cast<char>('a' + (ch - 'A'));

            if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'))
                normalized.push_back(ch);
        }
        return normalized;
    }

    int ParseMinimapToggleKey(const std::string& value)
    {
        const std::string normalized = NormalizeConfigValue(value);
        if (normalized.size() >= 2 && normalized[0] == 'f')
        {
            int number = 0;
            bool valid = true;
            for (std::size_t index = 1; index < normalized.size(); ++index)
            {
                const char ch = normalized[index];
                if (ch < '0' || ch > '9')
                {
                    valid = false;
                    break;
                }
                number = (number * 10) + (ch - '0');
            }
            if (valid && number >= 1 && number <= 24)
                return VK_F1 + number - 1;
        }

        if (normalized == "numpadmultiply" || normalized == "numpadstar" ||
            normalized == "multiply" || normalized == "asterisk" || normalized == "star")
            return VK_MULTIPLY;
        if (normalized == "insert" || normalized == "ins")
            return VK_INSERT;
        if (normalized == "delete" || normalized == "del")
            return VK_DELETE;
        if (normalized == "home")
            return VK_HOME;
        if (normalized == "end")
            return VK_END;
        if (normalized == "pageup" || normalized == "pgup")
            return VK_PRIOR;
        if (normalized == "pagedown" || normalized == "pgdn")
            return VK_NEXT;
        if (normalized == "backspace")
            return VK_BACK;

        return VK_F10;
    }

    bool ParseConfigBoolean(const std::string& value, bool fallback)
    {
        const std::string normalized = NormalizeConfigValue(value);
        if (normalized == "true" || normalized == "1" || normalized == "yes" ||
            normalized == "on" || normalized == "enabled" || normalized == "enable")
        {
            return true;
        }

        if (normalized == "false" || normalized == "0" || normalized == "no" ||
            normalized == "off" || normalized == "disabled" || normalized == "disable")
        {
            return false;
        }

        return fallback;
    }

    int ParseConfigInteger(const std::string& value, int fallback, int low, int high)
    {
        const std::string normalized = NormalizeConfigValue(value);
        if (normalized.empty())
            return fallback;

        int parsed = 0;
        for (char ch : normalized)
        {
            if (ch < '0' || ch > '9')
                return fallback;

            parsed = (parsed * 10) + (ch - '0');
            if (parsed > high)
                return high;
        }

        return ClampValue(parsed, low, high);
    }

    std::string MinimapToggleKeyName(int key)
    {
        if (key >= VK_F1 && key <= VK_F24)
        {
            std::ostringstream oss;
            oss << "F" << (key - VK_F1 + 1);
            return oss.str();
        }

        switch (key)
        {
        case VK_MULTIPLY:
            return "Numpad *";
        case VK_INSERT:
            return "Insert";
        case VK_DELETE:
            return "Delete";
        case VK_HOME:
            return "Home";
        case VK_END:
            return "End";
        case VK_PRIOR:
            return "PageUp";
        case VK_NEXT:
            return "PageDown";
        case VK_BACK:
            return "Backspace";
        default:
            return "F10";
        }
    }

    void RefreshMinimapConfig(ModContext* modContext, bool forceLog = false)
    {
        std::string configuredToggleKey = "F10";
        std::string toggleKeySource = "default";
        if (!TryReadMinimapConfigStringFromFile(modContext, "toggle_key", configuredToggleKey, toggleKeySource) &&
            modContext != nullptr && modContext->config.GetString)
        {
            configuredToggleKey = modContext->config.GetString("minimap_mod", "toggle_key", configuredToggleKey);
            toggleKeySource = "shroudtopia_config_api";
        }

        std::string configuredHeadingToggleKey = "F11";
        std::string headingToggleKeySource = "default";
        if (!TryReadMinimapConfigStringFromFile(modContext, "heading_toggle_key", configuredHeadingToggleKey, headingToggleKeySource) &&
            modContext != nullptr && modContext->config.GetString)
        {
            configuredHeadingToggleKey = modContext->config.GetString("minimap_mod", "heading_toggle_key", configuredHeadingToggleKey);
            headingToggleKeySource = "shroudtopia_config_api";
        }

        std::string configuredDebugLogging = "false";
        std::string debugLoggingSource = "default";
        if (!TryReadMinimapConfigStringFromFile(modContext, "debug_logging", configuredDebugLogging, debugLoggingSource) &&
            modContext != nullptr && modContext->config.GetString)
        {
            configuredDebugLogging = modContext->config.GetString("minimap_mod", "debug_logging", configuredDebugLogging);
            debugLoggingSource = "shroudtopia_config_api";
        }

        std::string configuredMapSampleStep = "2";
        std::string mapSampleStepSource = "default";
        if (!TryReadMinimapConfigStringFromFile(modContext, "map_sample_step", configuredMapSampleStep, mapSampleStepSource) &&
            modContext != nullptr && modContext->config.GetString)
        {
            configuredMapSampleStep = modContext->config.GetString("minimap_mod", "map_sample_step", configuredMapSampleStep);
            mapSampleStepSource = "shroudtopia_config_api";
        }

        std::string configuredMaxIcons = "64";
        std::string maxIconsSource = "default";
        if (!TryReadMinimapConfigStringFromFile(modContext, "max_icons", configuredMaxIcons, maxIconsSource) &&
            modContext != nullptr && modContext->config.GetString)
        {
            configuredMaxIcons = modContext->config.GetString("minimap_mod", "max_icons", configuredMaxIcons);
            maxIconsSource = "shroudtopia_config_api";
        }

        std::string configuredMapLight = "55";
        std::string mapLightSource = "default";
        if (!TryReadMinimapConfigStringFromFile(modContext, "map_light", configuredMapLight, mapLightSource) &&
            modContext != nullptr && modContext->config.GetString)
        {
            configuredMapLight = modContext->config.GetString("minimap_mod", "map_light", configuredMapLight);
            mapLightSource = "shroudtopia_config_api";
        }

        std::string configuredMapRenderer = "gpu";
        std::string mapRendererSource = "default";
        if (!TryReadMinimapConfigStringFromFile(modContext, "map_renderer", configuredMapRenderer, mapRendererSource) &&
            modContext != nullptr && modContext->config.GetString)
        {
            configuredMapRenderer = modContext->config.GetString("minimap_mod", "map_renderer", configuredMapRenderer);
            mapRendererSource = "shroudtopia_config_api";
        }

        std::string configuredLabelSize = std::to_string(MINIMAP_LABEL_SIZE_DEFAULT);
        std::string labelSizeSource = "default";
        if (!TryReadMinimapConfigStringFromFile(modContext, "label_font_size", configuredLabelSize, labelSizeSource) &&
            modContext != nullptr && modContext->config.GetString)
        {
            configuredLabelSize = modContext->config.GetString("minimap_mod", "label_font_size", configuredLabelSize);
            labelSizeSource = "shroudtopia_config_api";
        }
        {
            int labelSize = MINIMAP_LABEL_SIZE_DEFAULT;
            const std::string normalized = NormalizeConfigValue(configuredLabelSize);
            if (!normalized.empty() && normalized.find_first_not_of("0123456789") == std::string::npos)
                labelSize = std::atoi(normalized.c_str());
            g_minimapLabelFontSize.store(ClampValue(labelSize, 8, 40));
        }

        std::string configuredFogStrength = std::to_string(MINIMAP_FOG_STRENGTH_DEFAULT);
        std::string fogStrengthSource = "default";
        if (!TryReadMinimapConfigStringFromFile(modContext, "fog_strength", configuredFogStrength, fogStrengthSource) &&
            modContext != nullptr && modContext->config.GetString)
        {
            configuredFogStrength = modContext->config.GetString("minimap_mod", "fog_strength", configuredFogStrength);
            fogStrengthSource = "shroudtopia_config_api";
        }
        g_minimapFogStrength.store(ParseConfigInteger(configuredFogStrength, MINIMAP_FOG_STRENGTH_DEFAULT, 0, 100));

        std::string configuredMapTextureSize = std::to_string(MINIMAP_MAP_TEXTURE_SIZE_DEFAULT);
        std::string mapTextureSizeSource = "default";
        if (!TryReadMinimapConfigStringFromFile(modContext, "map_texture_size", configuredMapTextureSize, mapTextureSizeSource) &&
            modContext != nullptr && modContext->config.GetString)
        {
            configuredMapTextureSize = modContext->config.GetString("minimap_mod", "map_texture_size", configuredMapTextureSize);
            mapTextureSizeSource = "shroudtopia_config_api";
        }
        {
            int textureSize = MINIMAP_MAP_TEXTURE_SIZE_DEFAULT;
            const std::string normalized = NormalizeConfigValue(configuredMapTextureSize);
            if (!normalized.empty() && normalized.find_first_not_of("0123456789") == std::string::npos)
                textureSize = std::atoi(normalized.c_str());
            // Only power-of-two sizes the 8192 source can be halved down to.
            if (textureSize != 1024 && textureSize != 2048 && textureSize != 4096 && textureSize != 8192)
                textureSize = MINIMAP_MAP_TEXTURE_SIZE_DEFAULT;
            g_minimapMapTextureSize.store(textureSize);
        }

        std::string configuredMapFollow = "center";
        std::string mapFollowSource = "default";
        if (!TryReadMinimapConfigStringFromFile(modContext, "map_follow", configuredMapFollow, mapFollowSource) &&
            modContext != nullptr && modContext->config.GetString)
        {
            configuredMapFollow = modContext->config.GetString("minimap_mod", "map_follow", configuredMapFollow);
            mapFollowSource = "shroudtopia_config_api";
        }

        const int toggleKey = ParseMinimapToggleKey(configuredToggleKey);
        const int headingToggleKey = ParseMinimapToggleKey(configuredHeadingToggleKey);
        g_headingToggleKey.store(headingToggleKey);
        const std::string normalizedMapFollow = NormalizeConfigValue(configuredMapFollow);
        const bool staticView = normalizedMapFollow == "static" || normalizedMapFollow == "fixed" ||
            normalizedMapFollow == "free" || normalizedMapFollow == "map";
        const bool debugLogging = ParseConfigBoolean(configuredDebugLogging, false);
        const int mapSampleStep = ParseConfigInteger(configuredMapSampleStep, MINIMAP_DEFAULT_MAP_SAMPLE_STEP, 1, 4);
        const int maxIcons = ParseConfigInteger(configuredMaxIcons, MINIMAP_DEFAULT_MAX_DRAWN_POINTS, 8, 128);
        const int mapLight = ParseConfigInteger(configuredMapLight, 55, 0, 100);
        const std::string normalizedMapRenderer = NormalizeConfigValue(configuredMapRenderer);
        const bool mapGpu = !(normalizedMapRenderer == "cpu" || normalizedMapRenderer == "legacy" ||
            normalizedMapRenderer == "software" || normalizedMapRenderer == "false" || normalizedMapRenderer == "off");
        const int previousToggleKey = g_minimapToggleKey.exchange(toggleKey);
        const bool previousDebugLogging = g_debugLoggingEnabled.exchange(debugLogging);
        const int previousMapSampleStep = g_minimapMapSampleStep.exchange(mapSampleStep);
        const int previousMaxIcons = g_minimapMaxDrawnPoints.exchange(maxIcons);
        const int previousMapLight = g_minimapMapLight.exchange(mapLight);
        const bool previousMapGpu = g_minimapMapGpuEnabled.exchange(mapGpu);
        const bool previousStaticView = g_minimapStaticView.exchange(staticView);
        if (!forceLog &&
            previousToggleKey == toggleKey &&
            previousDebugLogging == debugLogging &&
            previousMapSampleStep == mapSampleStep &&
            previousMaxIcons == maxIcons &&
            previousMapLight == mapLight &&
            previousMapGpu == mapGpu &&
            previousStaticView == staticView)
        {
            return;
        }

        std::ostringstream oss;
        oss << "[Minimap] config"
            << " | toggle_key=" << MinimapToggleKeyName(toggleKey)
            << " | heading_toggle_key=" << MinimapToggleKeyName(headingToggleKey)
            << " | toggle_raw=" << configuredToggleKey
            << " | toggle_source=" << toggleKeySource
            << " | debug_logging=" << (debugLogging ? "on" : "off")
            << " | debug_logging_source=" << debugLoggingSource
            << " | map_sample_step=" << mapSampleStep
            << " | map_sample_step_source=" << mapSampleStepSource
            << " | max_icons=" << maxIcons
            << " | max_icons_source=" << maxIconsSource
            << " | map_light=" << mapLight
            << " | map_light_source=" << mapLightSource
            << " | map_renderer=" << (mapGpu ? "gpu" : "cpu")
            << " | map_renderer_source=" << mapRendererSource
            << " | label_font_size=" << g_minimapLabelFontSize.load()
            << " | label_font_size_source=" << labelSizeSource
            << " | map_texture_size=" << g_minimapMapTextureSize.load()
            << " | fog_strength=" << g_minimapFogStrength.load()
            << " | map_follow=" << (staticView ? "static" : "center")
            << " | map_follow_source=" << mapFollowSource;
        Log(oss.str());
    }

    int ComputeMinimapCenterY(std::uint32_t height, int radius, int marginY)
    {
        const int screenHeight = static_cast<int>(height);
        const int top = marginY + radius;
        const int safeTop = radius + 8;
        const int safeBottom = MaxValue(safeTop, screenHeight - radius - 8);

        // The minimap starts in the top-right corner; where it goes from there is up to
        // the Esc-menu drag (saved in minimap_layout.txt as an offset from this spot).
        return ClampValue(top, safeTop, safeBottom);
    }

    bool SafeRead(uintptr_t address, void* buffer, std::size_t size)
    {
        __try
        {
            std::memcpy(buffer, reinterpret_cast<const void*>(address), size);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    template <typename T>
    bool SafeReadValue(uintptr_t address, T& outValue)
    {
        return SafeRead(address, &outValue, sizeof(T));
    }

    std::size_t GetImageSize(uintptr_t moduleBase)
    {
        if (moduleBase == 0)
            return 0;

        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(moduleBase);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return 0;

        auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(moduleBase + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return 0;

        return static_cast<std::size_t>(nt->OptionalHeader.SizeOfImage);
    }

    uintptr_t RvaDistance(uintptr_t left, uintptr_t right)
    {
        return left > right ? left - right : right - left;
    }

    bool BytesMatchRva(uintptr_t rva, const std::uint8_t* expected, std::size_t size)
    {
        if (g_exeBase == 0 || g_exeImageSize == 0 || rva >= g_exeImageSize || size > g_exeImageSize - rva)
            return false;

        const auto* current = reinterpret_cast<const std::uint8_t*>(g_exeBase + rva);
        return std::memcmp(current, expected, size) == 0;
    }

    uintptr_t ResolvePatternRvaNear(uintptr_t preferredRva, const std::uint8_t* expected, std::size_t size, const char* label)
    {
        if (BytesMatchRva(preferredRva, expected, size))
            return preferredRva;

        if (g_exeBase == 0 || g_exeImageSize == 0 || size == 0 || size > g_exeImageSize)
        {
            Log(std::string("[Minimap] unable to scan hook pattern for ") + label);
            return 0;
        }

        const uintptr_t start = preferredRva > HOOK_PATTERN_SCAN_RADIUS ? preferredRva - HOOK_PATTERN_SCAN_RADIUS : 0;
        const uintptr_t rawEnd = preferredRva + HOOK_PATTERN_SCAN_RADIUS;
        const uintptr_t end = MinValue<uintptr_t>(rawEnd, static_cast<uintptr_t>(g_exeImageSize - size));
        uintptr_t best = 0;
        uintptr_t bestDistance = static_cast<uintptr_t>(-1);

        const auto* image = reinterpret_cast<const std::uint8_t*>(g_exeBase);
        for (uintptr_t rva = start; rva <= end; ++rva)
        {
            if (std::memcmp(image + rva, expected, size) != 0)
                continue;

            const uintptr_t distance = RvaDistance(rva, preferredRva);
            if (best == 0 || distance < bestDistance)
            {
                best = rva;
                bestDistance = distance;
            }
        }

        if (best == 0)
        {
            std::ostringstream oss;
            oss << "[Minimap] hook pattern missing for " << label
                << " | preferred=" << Hex(preferredRva);
            Log(oss.str());
            return 0;
        }

        std::ostringstream oss;
        oss << "[Minimap] hook pattern resolved for " << label
            << " | preferred=" << Hex(preferredRva)
            << " | resolved=" << Hex(best)
            << " | delta=" << static_cast<long long>(best) - static_cast<long long>(preferredRva);
        Log(oss.str());
        return best;
    }

    bool TryFindRelativeCallTargetRva(uintptr_t functionRva, std::size_t searchBytes, int ordinal, uintptr_t& outTargetRva)
    {
        outTargetRva = 0;
        if (functionRva == 0 || ordinal <= 0 || g_exeBase == 0 || g_exeImageSize == 0 || functionRva >= g_exeImageSize)
            return false;

        const std::size_t maxBytes = MinValue<std::size_t>(
            searchBytes,
            g_exeImageSize > functionRva ? g_exeImageSize - functionRva : 0
        );
        if (maxBytes < 5)
            return false;

        const auto* code = reinterpret_cast<const std::uint8_t*>(g_exeBase + functionRva);
        int found = 0;
        for (std::size_t offset = 0; offset + 5 <= maxBytes; ++offset)
        {
            if (code[offset] != 0xE8)
                continue;

            std::int32_t rel = 0;
            std::memcpy(&rel, code + offset + 1, sizeof(rel));
            const auto target = static_cast<std::int64_t>(functionRva) +
                static_cast<std::int64_t>(offset) + 5 + rel;
            if (target <= 0 || target >= static_cast<std::int64_t>(g_exeImageSize))
                continue;

            ++found;
            if (found == ordinal)
            {
                outTargetRva = static_cast<uintptr_t>(target);
                return true;
            }
        }

        return false;
    }

    bool ValidatePrologue(uintptr_t address, const std::uint8_t* expected, std::size_t size, const char* label)
    {
        std::vector<std::uint8_t> current(size, 0);
        if (!SafeRead(address, current.data(), current.size()))
        {
            Log(std::string("[Minimap] failed to read prologue for ") + label);
            return false;
        }

        if (std::memcmp(current.data(), expected, size) != 0)
        {
            std::ostringstream oss;
            oss << "[Minimap] prologue mismatch for " << label
                << " at " << Hex(address - g_exeBase);
            Log(oss.str());
            return false;
        }

        return true;
    }

    bool ActivateHook(Mem::Detour* hook)
    {
        if (hook == nullptr)
            return false;

        if (hook->active)
            return true;

        return hook->activate() || hook->active;
    }

    const char* HookActivationState(Mem::Detour* hook)
    {
        if (hook == nullptr)
            return "missing";

        return hook->active ? "active" : "failed";
    }

    std::vector<std::uint8_t> BuildEntryHookShellcode(const std::uint8_t* stolenBytes, std::size_t stolenSize)
    {
        std::vector<std::uint8_t> code;
        code.reserve(128);

        auto append = [&code](std::initializer_list<std::uint8_t> bytes)
        {
            code.insert(code.end(), bytes.begin(), bytes.end());
        };

        append({ 0x48, 0x83, 0xEC, 0x48 });               // sub rsp, 48h
        append({ 0x48, 0x89, 0x4C, 0x24, 0x20 });         // mov [rsp+20h], rcx
        append({ 0x48, 0x89, 0x54, 0x24, 0x28 });         // mov [rsp+28h], rdx
        append({ 0x4C, 0x89, 0x44, 0x24, 0x30 });         // mov [rsp+30h], r8
        append({ 0x4C, 0x89, 0x4C, 0x24, 0x38 });         // mov [rsp+38h], r9
        append({ 0x48, 0xB8 });                           // mov rax, imm64

        for (int i = 0; i < 8; ++i)
            code.push_back(0x00);

        append({ 0xFF, 0xD0 });                           // call rax
        append({ 0x48, 0x8B, 0x4C, 0x24, 0x20 });         // mov rcx, [rsp+20h]
        append({ 0x48, 0x8B, 0x54, 0x24, 0x28 });         // mov rdx, [rsp+28h]
        append({ 0x4C, 0x8B, 0x44, 0x24, 0x30 });         // mov r8, [rsp+30h]
        append({ 0x4C, 0x8B, 0x4C, 0x24, 0x38 });         // mov r9, [rsp+38h]
        append({ 0x48, 0x83, 0xC4, 0x48 });               // add rsp, 48h

        code.insert(code.end(), stolenBytes, stolenBytes + stolenSize);
        append({ 0xE9, 0x00, 0x00, 0x00, 0x00 });         // jmp back
        return code;
    }

    std::vector<std::uint8_t> BuildMapMarkerVisibilityLoopHookShellcode(const std::uint8_t* stolenBytes, std::size_t stolenSize)
    {
        std::vector<std::uint8_t> code;
        code.reserve(128);

        auto append = [&code](std::initializer_list<std::uint8_t> bytes)
        {
            code.insert(code.end(), bytes.begin(), bytes.end());
        };

        append({ 0x48, 0x83, 0xEC, 0x50 });               // sub rsp, 50h
        append({ 0x48, 0x89, 0x4C, 0x24, 0x20 });         // mov [rsp+20h], rcx
        append({ 0x48, 0x89, 0x54, 0x24, 0x28 });         // mov [rsp+28h], rdx
        append({ 0x4C, 0x89, 0x44, 0x24, 0x30 });         // mov [rsp+30h], r8
        append({ 0x4C, 0x89, 0x4C, 0x24, 0x38 });         // mov [rsp+38h], r9
        append({ 0x48, 0xB8 });                           // mov rax, imm64

        for (int i = 0; i < 8; ++i)
            code.push_back(0x00);

        append({ 0x48, 0x8D, 0x4D, 0xB8 });               // lea rcx, [rbp-48h]
        append({ 0x4C, 0x89, 0xF2 });                     // mov rdx, r14
        append({ 0xFF, 0xD0 });                           // call rax
        append({ 0x48, 0x8B, 0x4C, 0x24, 0x20 });         // mov rcx, [rsp+20h]
        append({ 0x48, 0x8B, 0x54, 0x24, 0x28 });         // mov rdx, [rsp+28h]
        append({ 0x4C, 0x8B, 0x44, 0x24, 0x30 });         // mov r8, [rsp+30h]
        append({ 0x4C, 0x8B, 0x4C, 0x24, 0x38 });         // mov r9, [rsp+38h]
        append({ 0x48, 0x83, 0xC4, 0x50 });               // add rsp, 50h

        code.insert(code.end(), stolenBytes, stolenBytes + stolenSize);
        append({ 0xE9, 0x00, 0x00, 0x00, 0x00 });         // jmp back
        return code;
    }

    template <std::size_t N>
    Mem::Detour* InstallEntryHook(uintptr_t rva, const std::array<std::uint8_t, N>& expected, void* handler, const char* label)
    {
        if (rva == 0)
        {
            Log(std::string("[Minimap] hook unavailable for ") + label);
            return nullptr;
        }

        const uintptr_t address = g_exeBase + rva;
        if (!ValidatePrologue(address, expected.data(), expected.size(), label))
            return nullptr;

        std::vector<std::uint8_t> shellcode = BuildEntryHookShellcode(expected.data(), expected.size());
        auto* detour = new Mem::Detour(address, shellcode.data(), shellcode.size(), false, expected.size() - 5);

        const std::size_t handlerImmediateOffset = 26;
        const std::size_t jumpImmediateOffset = shellcode.size() - 4;
        detour->shellcode->updateValue<std::uint64_t>(handlerImmediateOffset, reinterpret_cast<std::uint64_t>(handler));

        const uintptr_t jumpSource = detour->shellcode->data->address + jumpImmediateOffset + sizeof(std::uint32_t);
        const uintptr_t jumpTarget = address + expected.size();
        detour->shellcode->updateValue<std::uint32_t>(
            jumpImmediateOffset,
            static_cast<std::uint32_t>(jumpTarget - jumpSource)
        );

        return detour;
    }

    Mem::Detour* InstallMapMarkerVisibilityLoopHook(uintptr_t rva, void* handler)
    {
        if (rva == 0)
        {
            Log("[Minimap] hook unavailable for map_marker_visibility_loop");
            return nullptr;
        }

        const uintptr_t address = g_exeBase + rva;
        if (!ValidatePrologue(address, g_mapMarkerVisibilityLoopExpected.data(), g_mapMarkerVisibilityLoopExpected.size(), "map_marker_visibility_loop"))
            return nullptr;

        std::vector<std::uint8_t> shellcode = BuildMapMarkerVisibilityLoopHookShellcode(
            g_mapMarkerVisibilityLoopExpected.data(),
            g_mapMarkerVisibilityLoopExpected.size()
        );
        auto* detour = new Mem::Detour(address, shellcode.data(), shellcode.size(), false, g_mapMarkerVisibilityLoopExpected.size() - 5);

        const std::size_t handlerImmediateOffset = 26;
        const std::size_t jumpImmediateOffset = shellcode.size() - 4;
        detour->shellcode->updateValue<std::uint64_t>(handlerImmediateOffset, reinterpret_cast<std::uint64_t>(handler));

        const uintptr_t jumpSource = detour->shellcode->data->address + jumpImmediateOffset + sizeof(std::uint32_t);
        const uintptr_t jumpTarget = address + g_mapMarkerVisibilityLoopExpected.size();
        detour->shellcode->updateValue<std::uint32_t>(
            jumpImmediateOffset,
            static_cast<std::uint32_t>(jumpTarget - jumpSource)
        );

        return detour;
    }

    float FixedToWorld(std::int64_t value)
    {
        return static_cast<float>(static_cast<double>(value) * FIXED_32_32_TO_WORLD);
    }

    std::int64_t WorldToFixed(float value)
    {
        return static_cast<std::int64_t>(static_cast<double>(value) / FIXED_32_32_TO_WORLD);
    }

    bool SameWaypoints(const std::vector<CapturedWaypoint>& left, const std::vector<CapturedWaypoint>& right)
    {
        if (left.size() != right.size())
            return false;

        for (std::size_t index = 0; index < left.size(); ++index)
        {
            if (left[index].id != right[index].id ||
                left[index].x != right[index].x ||
                left[index].y != right[index].y ||
                left[index].z != right[index].z ||
                std::memcmp(left[index].raw, right[index].raw, sizeof(left[index].raw)) != 0)
            {
                return false;
            }
        }

        return true;
    }

    bool SameNearbyMarkers(const std::vector<CapturedNearbyMarker>& left, const std::vector<CapturedNearbyMarker>& right)
    {
        if (left.size() != right.size())
            return false;

        for (std::size_t index = 0; index < left.size(); ++index)
        {
            if (left[index].kind != right[index].kind ||
                left[index].hasWorldPosition != right[index].hasWorldPosition ||
                left[index].x != right[index].x ||
                left[index].y != right[index].y ||
                left[index].z != right[index].z ||
                std::memcmp(left[index].raw, right[index].raw, sizeof(left[index].raw)) != 0)
            {
                return false;
            }
        }

        return true;
    }

    bool SameVisibleMapMarkerIdentity(const CapturedMapMarkerVisibility& left, const CapturedMapMarkerVisibility& right)
    {
        if (left.stateAddress != 0 && right.stateAddress != 0 && left.stateAddress == right.stateAddress)
            return true;

        if (left.markerAddress != 0 && right.markerAddress != 0 && left.markerAddress == right.markerAddress)
            return true;

        if (!left.hasWorldPosition || !right.hasWorldPosition)
            return false;

        const float dx = FixedToWorld(left.x) - FixedToWorld(right.x);
        const float dz = FixedToWorld(left.z) - FixedToWorld(right.z);
        if (dx * dx + dz * dz > 16.0f)
            return false;

        return left.kind == right.kind || left.markerId == right.markerId;
    }

    bool SameWaypointPosition(const CapturedWaypoint& left, const CapturedWaypoint& right)
    {
        const float dx = FixedToWorld(left.x) - FixedToWorld(right.x);
        const float dz = FixedToWorld(left.z) - FixedToWorld(right.z);
        return std::abs(dx) < 2.0f && std::abs(dz) < 2.0f;
    }

    void PushWaypointUnique(std::vector<CapturedWaypoint>& waypoints, const CapturedWaypoint& waypoint)
    {
        for (const CapturedWaypoint& existing : waypoints)
        {
            if (SameWaypointPosition(existing, waypoint))
                return;
        }

        waypoints.push_back(waypoint);
    }

    bool IsPlausibleWorldPosition(float x, float y, float z)
    {
        return std::abs(x) < 30000.0f &&
            std::abs(z) < 30000.0f &&
            y > -5000.0f &&
            y < 8000.0f &&
            (std::abs(x) > 1.0f || std::abs(z) > 1.0f);
    }

    bool TryDecodeFixedWorldPosition(const std::uint64_t* raw, std::int64_t& outX, std::int64_t& outY, std::int64_t& outZ)
    {
        for (int first = 0; first <= 2; ++first)
        {
            const std::int64_t x = static_cast<std::int64_t>(raw[first + 0]);
            const std::int64_t y = static_cast<std::int64_t>(raw[first + 1]);
            const std::int64_t z = static_cast<std::int64_t>(raw[first + 2]);
            if (IsPlausibleWorldPosition(FixedToWorld(x), FixedToWorld(y), FixedToWorld(z)))
            {
                outX = x;
                outY = y;
                outZ = z;
                return true;
            }
        }

        return false;
    }

    void NoteWorldData(const char* reason)
    {
        const DWORD now = GetTickCount();
        g_lastWorldDataTick.store(now);

        if (!g_worldSessionReady.exchange(true))
        {
            std::ostringstream oss;
            oss << "[Minimap] world session ready | reason=" << reason;
            Log(oss.str());
        }
    }

    bool HasRecentWorldData()
    {
        const DWORD lastWorldDataTick = g_lastWorldDataTick.load();
        return lastWorldDataTick != 0 && GetTickCount() - lastWorldDataTick <= WORLD_DATA_STALE_MS;
    }

    bool HasFreshPlayerPosition()
    {
        const DWORD now = GetTickCount();
        std::lock_guard<std::mutex> lock(g_playerPositionMutex);
        return g_playerPosition.valid && now - g_playerPosition.lastUpdateTick <= WORLD_DATA_STALE_MS;
    }

    bool ShouldDrawMinimapInWorld()
    {
        if (!g_minimapVisible.load())
            return false;

        CURSORINFO cursorInfo{};
        cursorInfo.cbSize = sizeof(cursorInfo);
        if (GetCursorInfo(&cursorInfo) && (cursorInfo.flags & CURSOR_SHOWING) != 0 && !g_layoutEditMode.load())
            return false;

        return HasFreshPlayerPosition();
    }

    std::string ResolveGameLogPath()
    {
        char modulePath[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameA(nullptr, modulePath, static_cast<DWORD>(sizeof(modulePath)));
        if (length == 0 || length >= sizeof(modulePath))
            return "enshrouded.log";

        char* slash = std::strrchr(modulePath, '\\');
        if (slash == nullptr)
            return "enshrouded.log";

        *(slash + 1) = '\0';
        return std::string(modulePath) + "enshrouded.log";
    }

    bool ReadFileTail(const std::string& path, DWORD maxBytes, std::string& outText)
    {
        HANDLE file = CreateFileA(
            path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

        if (file == INVALID_HANDLE_VALUE)
            return false;

        LARGE_INTEGER fileSize{};
        if (!GetFileSizeEx(file, &fileSize))
        {
            CloseHandle(file);
            return false;
        }

        const LONGLONG bytesToRead64 = MinValue<LONGLONG>(fileSize.QuadPart, static_cast<LONGLONG>(maxBytes));
        if (bytesToRead64 <= 0)
        {
            CloseHandle(file);
            outText.clear();
            return true;
        }

        LARGE_INTEGER distance{};
        distance.QuadPart = fileSize.QuadPart - bytesToRead64;
        if (!SetFilePointerEx(file, distance, nullptr, FILE_BEGIN))
        {
            CloseHandle(file);
            return false;
        }

        outText.assign(static_cast<std::size_t>(bytesToRead64), '\0');
        DWORD bytesRead = 0;
        const BOOL ok = ReadFile(file, &outText[0], static_cast<DWORD>(outText.size()), &bytesRead, nullptr);
        CloseHandle(file);
        if (!ok)
            return false;

        outText.resize(bytesRead);
        return true;
    }

    bool ReadWholeTextFile(const std::string& path, std::string& outText)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return false;

        std::ostringstream buffer;
        buffer << file.rdbuf();
        outText = buffer.str();
        return true;
    }

    std::string ResolveShroudtopiaConfigPath(ModContext* modContext)
    {
        if (!g_shroudtopiaConfigPath.empty())
            return g_shroudtopiaConfigPath;

        if (modContext != nullptr && !modContext->shroudtopia.config_file.empty())
            return modContext->shroudtopia.config_file;

        char modulePath[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameA(nullptr, modulePath, static_cast<DWORD>(sizeof(modulePath)));
        if (length == 0 || length >= sizeof(modulePath))
            return {};

        char* slash = std::strrchr(modulePath, '\\');
        if (slash == nullptr)
            return "shroudtopia.json";

        *(slash + 1) = '\0';
        return std::string(modulePath) + "shroudtopia.json";
    }

    bool TryFindJsonObjectBlock(const std::string& text, const char* key, std::size_t& outStart, std::size_t& outEnd)
    {
        const std::string keyToken = std::string("\"") + key + "\"";
        const std::size_t keyPos = text.find(keyToken);
        if (keyPos == std::string::npos)
            return false;

        const std::size_t colon = text.find(':', keyPos + keyToken.size());
        if (colon == std::string::npos)
            return false;

        const std::size_t openBrace = text.find('{', colon + 1);
        if (openBrace == std::string::npos)
            return false;

        bool inString = false;
        bool escaped = false;
        int depth = 0;
        for (std::size_t index = openBrace; index < text.size(); ++index)
        {
            const char ch = text[index];
            if (inString)
            {
                if (escaped)
                    escaped = false;
                else if (ch == '\\')
                    escaped = true;
                else if (ch == '"')
                    inString = false;
                continue;
            }

            if (ch == '"')
                inString = true;
            else if (ch == '{')
                ++depth;
            else if (ch == '}')
            {
                --depth;
                if (depth == 0)
                {
                    outStart = openBrace;
                    outEnd = index + 1;
                    return true;
                }
            }
        }

        return false;
    }

    bool TryFindJsonStringValue(const std::string& text, std::size_t start, std::size_t end, const char* key, std::string& outValue)
    {
        const std::string keyToken = std::string("\"") + key + "\"";
        const std::size_t keyPos = text.find(keyToken, start);
        if (keyPos == std::string::npos || keyPos >= end)
            return false;

        const std::size_t colon = text.find(':', keyPos + keyToken.size());
        if (colon == std::string::npos || colon >= end)
            return false;

        std::size_t valueStart = colon + 1;
        while (valueStart < end && (text[valueStart] == ' ' || text[valueStart] == '\t' || text[valueStart] == '\r' || text[valueStart] == '\n'))
            ++valueStart;

        if (valueStart >= end)
            return false;

        if (text[valueStart] == '"')
        {
            std::string value;
            bool escaped = false;
            for (std::size_t index = valueStart + 1; index < end; ++index)
            {
                const char ch = text[index];
                if (escaped)
                {
                    value.push_back(ch);
                    escaped = false;
                }
                else if (ch == '\\')
                    escaped = true;
                else if (ch == '"')
                {
                    outValue = value;
                    return true;
                }
                else
                    value.push_back(ch);
            }
            return false;
        }

        std::size_t valueEnd = valueStart;
        while (valueEnd < end && text[valueEnd] != ',' && text[valueEnd] != '}' && text[valueEnd] != '\r' && text[valueEnd] != '\n')
            ++valueEnd;

        while (valueEnd > valueStart && (text[valueEnd - 1] == ' ' || text[valueEnd - 1] == '\t'))
            --valueEnd;

        if (valueEnd <= valueStart)
            return false;

        outValue.assign(text.begin() + valueStart, text.begin() + valueEnd);
        return true;
    }

    bool TryReadMinimapConfigStringFromFile(ModContext* modContext, const char* key, std::string& outValue, std::string& outSource)
    {
        const std::string configPath = ResolveShroudtopiaConfigPath(modContext);
        if (configPath.empty() || key == nullptr || key[0] == '\0')
            return false;

        std::string text;
        if (!ReadWholeTextFile(configPath, text))
            return false;

        std::size_t blockStart = 0;
        std::size_t blockEnd = 0;
        if (!TryFindJsonObjectBlock(text, "minimap_mod", blockStart, blockEnd))
            return false;

        std::string value;
        if (!TryFindJsonStringValue(text, blockStart, blockEnd, key, value))
            return false;

        outValue = value;
        outSource = configPath;
        return true;
    }

    std::size_t LatestPatternPosition(const std::string& text, std::initializer_list<const char*> patterns)
    {
        std::size_t latest = std::string::npos;
        for (const char* pattern : patterns)
        {
            const std::size_t found = text.rfind(pattern);
            if (found != std::string::npos && (latest == std::string::npos || found > latest))
                latest = found;
        }

        return latest;
    }

    void ResetWorldSessionFromLog()
    {
        const bool wasOnline = g_gameSessionOnline.exchange(false);
        const bool wasReady = g_worldSessionReady.exchange(false);
        g_lastWorldDataTick.store(0);
        {
            // The next world brings its own grid; do not draw this one over it.
            std::lock_guard<std::mutex> lock(g_fogGridMutex);
            g_fogGrid.valid = false;
            std::vector<std::uint8_t>().swap(g_fogGrid.cells);
            ++g_fogGrid.version;
        }
        g_fogGridLogged.store(false);

        {
            std::lock_guard<std::mutex> lock(g_playerPositionMutex);
            g_playerPosition = {};
        }
        {
            std::lock_guard<std::mutex> lock(g_visibleMapMarkerMutex);
            g_visibleMapMarkers.clear();
        }
        if (wasOnline || wasReady)
            Log("[Minimap] world session closed | reason=game_session_log");
    }

    void PollGameSessionLog()
    {
        const DWORD now = GetTickCount();
        if (now - g_lastSessionLogPollTick < SESSION_LOG_POLL_MS)
            return;

        g_lastSessionLogPollTick = now;
        if (g_gameLogPath.empty())
            g_gameLogPath = ResolveGameLogPath();

        std::string tail;
        if (!ReadFileTail(g_gameLogPath, SESSION_LOG_TAIL_BYTES, tail))
            return;

        const std::size_t joined = LatestPatternPosition(tail, {
            "[SessionPlayer] finished transition from 'Local' to 'Local_InSession'",
            "[Session] finished transition from 'Lobby' to 'Client_Online'",
            "[play] start creation step LoadingScreenDone"
        });
        const std::size_t left = LatestPatternPosition(tail, {
            "[SessionPlayer] finished transition from 'Local_InSession' to 'Local'",
            "[Session] finished transition from 'Client_Online' to 'Lobby'",
            "[play] start destruction step Session"
        });

        if (joined != std::string::npos && (left == std::string::npos || joined > left))
        {
            const bool wasOnline = g_gameSessionOnline.exchange(true);
            if (!wasOnline)
            {
                g_minimapVisible.store(true);
                Log("[Minimap] game session detected from Enshrouded log");
            }
            return;
        }

        if (left != std::string::npos && (joined == std::string::npos || left > joined))
            ResetWorldSessionFromLog();
    }

    const char* PlayerPositionChannelName(std::uint32_t channel)
    {
        switch (channel)
        {
        case 1:
            return "ui_source";
        case 2:
            return "ui_local30";
        case 3:
            return "ui_state_camera";
        case 4:
            return "ui_state_camera_new";
        case 5:
            return "ui_state_position_exact";
        case 6:
            return "live_position_tracker";
        case 7:
            return "ui_state_live_position";
        case 20:
            return "cached_client_camera";
        case 21:
            return "waypoint_root_camera";
        case 22:
            return "waypoint_child_camera";
        case 23:
            return "waypoint_direct_camera";
        case 24:
            return "render_context_camera";
        case 25:
            return "render_child_camera";
        case 26:
            return "ui_state_full_camera_new";
        case 27:
            return "ui_state_full_camera_old";
        case 28:
            return "ui_source_full_camera";
        case 29:
            return "signature_scan_camera";
        default:
            return "unknown";
        }
    }

    bool TryScoreAgainstWorldAnchors(float worldX, float worldZ, float& outScore)
    {
        bool hasAnchor = false;
        float bestScore = 1000000000000.0f;

        {
            std::lock_guard<std::mutex> lock(g_waypointMutex);
            for (const CapturedWaypoint& waypoint : g_waypoints)
            {
                const float dx = FixedToWorld(waypoint.x) - worldX;
                const float dz = FixedToWorld(waypoint.z) - worldZ;
                bestScore = MinValue(bestScore, dx * dx + dz * dz);
                hasAnchor = true;
            }
        }

        {
            std::lock_guard<std::mutex> lock(g_nearbyMarkerMutex);
            for (const CapturedNearbyMarker& marker : g_nearbyMarkers)
            {
                if (!marker.hasWorldPosition)
                    continue;

                const float dx = FixedToWorld(marker.x) - worldX;
                const float dz = FixedToWorld(marker.z) - worldZ;
                bestScore = MinValue(bestScore, dx * dx + dz * dz);
                hasAnchor = true;
            }
        }

        outScore = hasAnchor ? bestScore : 0.0f;
        return hasAnchor;
    }

    bool IsAcceptablePlayerPositionCandidate(float worldX, float worldZ, float& outScore)
    {
        // Reject axis-edge decoys such as (0, 0, 256.079): real player positions sit
        // well inside the map square on both axes.
        if (worldX < 1.0f || worldZ < 1.0f)
            return false;

        const bool hasAnchor = TryScoreAgainstWorldAnchors(worldX, worldZ, outScore);
        if (hasAnchor)
            return outScore <= 12000.0f * 12000.0f;

        if (worldX > REAL_MAP_WORLD_SIZE + 512.0f ||
            worldZ > REAL_MAP_WORLD_SIZE + 512.0f)
        {
            return false;
        }

        const float dx = worldX - REAL_MAP_WORLD_SIZE * 0.5f;
        const float dz = worldZ - REAL_MAP_WORLD_SIZE * 0.5f;
        outScore = dx * dx + dz * dz;
        return true;
    }

    struct PlayerPositionCandidate
    {
        std::int64_t x = 0;
        std::int64_t y = 0;
        std::int64_t z = 0;
        uintptr_t source = 0;
        std::uint32_t offset = 0;
        std::uint32_t channel = 0;
        float score = 0.0f;
        float headingRadians = 0.0f;
        bool hasHeading = false;
        bool valid = false;
    };

    bool TryCapturePlayerPositionFromBlock(uintptr_t base, std::uint32_t channel, std::size_t scanBytes, PlayerPositionCandidate& best)
    {
        if (base == 0 || scanBytes < sizeof(std::uint64_t) * 5)
            return false;

        bool captured = false;
        for (std::size_t offset = 0; offset + sizeof(std::uint64_t) * 5 <= scanBytes; offset += sizeof(std::uint64_t))
        {
            std::uint64_t raw[5] = {};
            if (!SafeRead(base + offset, raw, sizeof(raw)))
                continue;

            std::int64_t x = 0;
            std::int64_t y = 0;
            std::int64_t z = 0;
            if (!TryDecodeFixedWorldPosition(raw, x, y, z))
                continue;

            float score = 0.0f;
            if (!IsAcceptablePlayerPositionCandidate(FixedToWorld(x), FixedToWorld(z), score))
                continue;

            if (!best.valid || score < best.score)
            {
                best.x = x;
                best.y = y;
                best.z = z;
                best.source = base;
                best.offset = static_cast<std::uint32_t>(offset);
                best.channel = channel;
                best.score = score;
                best.valid = true;
            }
            captured = true;
        }

        return captured;
    }

    CapturedPlayerPosition g_lastExactPosition{};
    DWORD g_directFeedPublishTick = 0;       // GetTickCount() of the last live (6/7) publish
    std::atomic<std::uint32_t> g_foreignUiStateReads{ 0 };

    // Milliseconds since `then`; 0 when `then` is not in the past. Feeds are captured
    // on different threads, so a sample tick can be a few ms newer than "now".
    DWORD TicksSince(DWORD now, DWORD then)
    {
        return now >= then ? now - then : 0;
    }

    float WrapAngleRadians(float angle)
    {
        while (angle > 3.14159265f)
            angle -= 6.28318531f;
        while (angle < -3.14159265f)
            angle += 6.28318531f;
        return angle;
    }

    struct PositionFeedStats
    {
        std::uint32_t publishes[32] = {};
        std::uint32_t changes[32] = {};
        std::int64_t lastX[32] = {};
        std::int64_t lastZ[32] = {};
        DWORD windowStart = 0;
    };
    PositionFeedStats g_positionFeedStats;

    void NotePositionFeedSample(const CapturedPlayerPosition& position)
    {
        const std::uint32_t channel = position.channel < 32 ? position.channel : 31;
        PositionFeedStats& stats = g_positionFeedStats;
        ++stats.publishes[channel];
        if (stats.lastX[channel] != position.x || stats.lastZ[channel] != position.z)
        {
            ++stats.changes[channel];
            stats.lastX[channel] = position.x;
            stats.lastZ[channel] = position.z;
        }

        const DWORD now = position.lastUpdateTick;
        if (stats.windowStart == 0 || now < stats.windowStart)
            stats.windowStart = now;
        if (now - stats.windowStart < 5000)
            return;

        if (g_debugLoggingEnabled.load())
        {
            std::ostringstream oss;
            oss << "[Minimap] position feed rates | window_ms=" << (now - stats.windowStart);
            for (std::uint32_t index = 0; index < 32; ++index)
            {
                if (stats.publishes[index] == 0)
                    continue;
                oss << " | " << PlayerPositionChannelName(index) << "(" << index << ")"
                    << " publish=" << stats.publishes[index]
                    << " moved=" << stats.changes[index];
            }
            oss << " | foreign_ui_state_reads=" << g_foreignUiStateReads.exchange(0);
            Log(oss.str());
        }

        std::memset(stats.publishes, 0, sizeof(stats.publishes));
        std::memset(stats.changes, 0, sizeof(stats.changes));
        stats.windowStart = now;
    }

    // Movement heading: the facing direction on the world X/Z plane is (sin h, cos h),
    // so the direction of travel from A to B is h = atan2(dx, dz). The position feed
    // updates ~80 times a second in steps of a few centimetres, so the heading is taken
    // over the last ~0.5 m travelled instead of per sample (per-sample deltas are pure
    // noise). Standing still keeps the last direction. Guarded by g_playerPositionMutex.
    // Step 0.5 m / blend 0.7 was tuned offline: a 90-degree turn at walking speed lands
    // within 15 degrees in ~0.2 s, and walking straight wobbles by under ~5 degrees.
    struct MovementHeadingState
    {
        bool anchorValid = false;
        float anchorX = 0.0f;
        float anchorZ = 0.0f;
        std::uint32_t anchorChannel = 0;
        bool valid = false;
        float heading = 0.0f;
    };
    MovementHeadingState g_movementHeading;
    constexpr float MOVEMENT_HEADING_STEP = 0.5f;       // world units travelled per update
    constexpr float MOVEMENT_HEADING_TELEPORT = 60.0f;  // larger jumps re-anchor only

    void ResetMovementHeadingLocked()
    {
        g_movementHeading = MovementHeadingState{};
    }

    void ResetHeadingState()
    {
        std::lock_guard<std::mutex> lock(g_playerPositionMutex);
        ResetMovementHeadingLocked();
    }

    void UpdateMovementHeadingLocked(float x, float z, std::uint32_t channel)
    {
        MovementHeadingState& state = g_movementHeading;
        // A different feed may sit on a slightly different point of the player: start
        // over from it rather than reading the gap between the two as movement.
        if (!state.anchorValid || state.anchorChannel != channel)
        {
            state.anchorValid = true;
            state.anchorX = x;
            state.anchorZ = z;
            state.anchorChannel = channel;
            return;
        }

        const float dx = x - state.anchorX;
        const float dz = z - state.anchorZ;
        const float distanceSq = dx * dx + dz * dz;
        if (distanceSq < MOVEMENT_HEADING_STEP * MOVEMENT_HEADING_STEP)
            return;

        state.anchorX = x;
        state.anchorZ = z;
        if (distanceSq > MOVEMENT_HEADING_TELEPORT * MOVEMENT_HEADING_TELEPORT)
            return;   // fast travel / respawn: not a direction

        const float target = std::atan2(dx, dz);
        if (!state.valid)
        {
            state.heading = target;
            state.valid = true;
        }
        else
        {
            // Partial blend per step: strafing and zig-zags wobble less, a real turn
            // still lands within two or three steps.
            state.heading = WrapAngleRadians(state.heading + WrapAngleRadians(target - state.heading) * 0.7f);
        }
    }

    void PublishPlayerPosition(const CapturedPlayerPosition& position)
    {
        {
            std::lock_guard<std::mutex> lock(g_playerPositionMutex);
            NotePositionFeedSample(position);

            // Direct feeds: the UI-state live slot (7), the memory tracker (6) and the
            // fixed UI-state slot (5). They also drive the movement heading.
            const bool isDirectFeed = position.channel == 5 || position.channel == 6 || position.channel == 7;
            if (isDirectFeed)
            {
                g_lastExactPosition = position;
                if (g_headingEnabled.load())
                    UpdateMovementHeadingLocked(FixedToWorld(position.x), FixedToWorld(position.z), position.channel);
            }

            const DWORD publishNow = GetTickCount();
            if (position.channel == 6 || position.channel == 7)
                g_directFeedPublishTick = publishNow;

            // A live direct feed is the authority; the heuristic block scans only fill in
            // while it is silent (letting both move the map made it flicker).
            const bool directLive = g_directFeedPublishTick != 0 && TicksSince(publishNow, g_directFeedPublishTick) < 1000;
            if (isDirectFeed || !directLive)
            {
                CapturedPlayerPosition merged = position;
                merged.hasHeading = false;
                merged.headingRadians = 0.0f;
                g_playerPosition = merged;
            }
        }

        NoteWorldData("player_position");

        if (!g_debugLoggingEnabled.load())
            return;

        const DWORD now = position.lastUpdateTick;
        if (now - g_lastPlayerPositionSummaryTick < 3000)
            return;

        g_lastPlayerPositionSummaryTick = now;

        std::ostringstream oss;
        oss << "[Minimap] player/map position feed"
            << " | channel=" << PlayerPositionChannelName(position.channel)
            << " | source=" << Hex(position.source)
            << " | offset=0x" << std::hex << position.offset << std::dec
            << " | world=("
            << FixedToWorld(position.x) << ", "
            << FixedToWorld(position.y) << ", "
            << FixedToWorld(position.z) << ")";
        Log(oss.str());
    }

    bool TryGetPlayerPosition(CapturedPlayerPosition& outPosition)
    {
        std::lock_guard<std::mutex> lock(g_playerPositionMutex);
        const DWORD now = GetTickCount();
        if (!g_playerPosition.valid || TicksSince(now, g_playerPosition.lastUpdateTick) > WORLD_DATA_STALE_MS)
            return false;

        outPosition = g_playerPosition;
        // The view direction is the direction of travel (see UpdateMovementHeadingLocked).
        outPosition.hasHeading = g_headingEnabled.load() && g_movementHeading.valid;
        outPosition.headingRadians = outPosition.hasHeading ? g_movementHeading.heading : 0.0f;
        return true;
    }

    bool PublishBestPlayerPosition(const PlayerPositionCandidate& best);
    bool IsLikelyRuntimePointer(uintptr_t value);
    bool IsOnMapWorldPosition(float x, float y, float z);
    std::uint8_t* ResolveWaypointsUiState(const WaypointsUiIterationRecord& record);

    // Direct read of the live player position verified against the running May-24-2026
    // client (state+0x1DCB8 tracked the player smoothly during live memory probing).
    std::atomic<DWORD> g_liveTrackerPublishTick{ 0 };

    // Raw state of the UI-state slot, kept even while it is not published: the live
    // tracker uses it as its search origin and rescans when it changes (load/teleport).
    struct ExactSlotRaw
    {
        std::int64_t x = 0;
        std::int64_t y = 0;
        std::int64_t z = 0;
    };
    ExactSlotRaw g_exactSlotRaw;                         // guarded by g_playerPositionMutex
    std::atomic<bool> g_exactSlotValid{ false };
    std::atomic<std::uint32_t> g_exactSlotSerial{ 0 };
    std::atomic<DWORD> g_exactSlotChangeTick{ 0 };
    std::atomic<uintptr_t> g_uiStateAddress{ 0 };

    // Live player position inside the same UI state object (float32 x, y, z). Found on
    // the September 2026 client by the live tracker: these copies move with the
    // player every frame while the fixed-point slot above stays put.
    constexpr std::size_t UI_STATE_LIVE_POSITION_OFFSETS[] = { 0xE0, 0xB50, 0x3730 };
    std::atomic<DWORD> g_uiLivePositionTick{ 0 };
    std::atomic<std::size_t> g_uiLivePositionLoggedOffset{ 0 };

    bool TryReadUiLivePosition(std::uint8_t* state, float& x, float& y, float& z, std::size_t& usedOffset)
    {
        for (std::size_t offset : UI_STATE_LIVE_POSITION_OFFSETS)
        {
            float v[3] = {};
            if (!SafeRead(reinterpret_cast<uintptr_t>(state) + offset, v, sizeof(v)))
                continue;
            if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2]))
                continue;
            if (!IsOnMapWorldPosition(v[0], v[1], v[2]))
                continue;
            x = v[0];
            y = v[1];
            z = v[2];
            usedOffset = offset;
            return true;
        }
        return false;
    }

    bool TryPublishUiLivePosition(std::uint8_t* state)
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        std::size_t offset = 0;
        if (!TryReadUiLivePosition(state, x, y, z, offset))
            return false;

        CapturedPlayerPosition position{};
        position.x = WorldToFixed(x);
        position.y = WorldToFixed(y);
        position.z = WorldToFixed(z);
        position.source = reinterpret_cast<uintptr_t>(state);
        position.offset = static_cast<std::uint32_t>(offset);
        position.channel = 7;
        position.lastUpdateTick = GetTickCount();
        position.valid = true;
        g_uiLivePositionTick.store(position.lastUpdateTick);
        PublishPlayerPosition(position);

        if (g_uiLivePositionLoggedOffset.exchange(offset) != offset)
        {
            std::ostringstream oss;
            oss << "[Minimap] ui_state live position slot | state=" << Hex(reinterpret_cast<uintptr_t>(state))
                << " | offset=" << Hex(offset)
                << " | world=(" << x << ", " << y << ", " << z << ")";
            Log(oss.str());
        }
        return true;
    }

    bool UiLivePositionFresh()
    {
        const DWORD tick = g_uiLivePositionTick.load();
        return tick != 0 && GetTickCount() - tick < 3000;
    }

    // In multiplayer the UI hooks are also handed other players' UI states, whose live
    // slot holds *their* position; publishing those made the own marker jump between
    // players. Stick to one state (the first one after load is the local player's) and
    // only move on when it has been silent for a while (world change / respawn).
    constexpr DWORD UI_STATE_LOCK_TIMEOUT_MS = 3000;
    std::atomic<uintptr_t> g_lockedUiState{ 0 };
    std::atomic<DWORD> g_lockedUiStateTick{ 0 };

    bool AcceptUiState(std::uint8_t* state)
    {
        const uintptr_t address = reinterpret_cast<uintptr_t>(state);
        const DWORD now = GetTickCount();
        const uintptr_t locked = g_lockedUiState.load();
        const DWORD lockedTick = g_lockedUiStateTick.load();
        if (locked == address)
        {
            g_lockedUiStateTick.store(now);
            return true;
        }

        if (locked != 0 && TicksSince(now, lockedTick) < UI_STATE_LOCK_TIMEOUT_MS)
        {
            g_foreignUiStateReads.fetch_add(1);
            static std::atomic<uintptr_t> lastLoggedForeign{ 0 };
            if (lastLoggedForeign.exchange(address) != address)
            {
                std::ostringstream oss;
                oss << "[Minimap] ignoring another UI state (remote player?)"
                    << " | locked=" << Hex(locked)
                    << " | other=" << Hex(address);
                Log(oss.str());
            }
            return false;
        }

        g_lockedUiState.store(address);
        g_lockedUiStateTick.store(now);
        std::ostringstream oss;
        oss << "[Minimap] UI state locked | state=" << Hex(address)
            << " | previous=" << Hex(locked);
        Log(oss.str());
        return true;
    }

    bool TryPublishExactUiStatePosition(std::uint8_t* state)
    {
        if (state == nullptr)
            return false;
        if (!AcceptUiState(state))
            return false;

        g_uiStateAddress.store(reinterpret_cast<uintptr_t>(state));
        const bool livePublished = TryPublishUiLivePosition(state);

        std::int64_t x = 0;
        std::int64_t y = 0;
        std::int64_t z = 0;
        if (!SafeReadValue(reinterpret_cast<uintptr_t>(state + UI_STATE_PLAYER_POSITION_MAY24 + 0x00), x) ||
            !SafeReadValue(reinterpret_cast<uintptr_t>(state + UI_STATE_PLAYER_POSITION_MAY24 + 0x08), y) ||
            !SafeReadValue(reinterpret_cast<uintptr_t>(state + UI_STATE_PLAYER_POSITION_MAY24 + 0x10), z))
        {
            return livePublished;
        }

        if (!IsOnMapWorldPosition(FixedToWorld(x), FixedToWorld(y), FixedToWorld(z)))
            return livePublished;

        {
            std::lock_guard<std::mutex> lock(g_playerPositionMutex);
            const float dx = FixedToWorld(x) - FixedToWorld(g_exactSlotRaw.x);
            const float dz = FixedToWorld(z) - FixedToWorld(g_exactSlotRaw.z);
            if (!g_exactSlotValid.load() || dx * dx + dz * dz > 1.0f)
            {
                g_exactSlotRaw.x = x;
                g_exactSlotRaw.y = y;
                g_exactSlotRaw.z = z;
                g_exactSlotValid.store(true);
                g_exactSlotSerial.fetch_add(1);
                g_exactSlotChangeTick.store(GetTickCount());
            }
        }

        // The live slot is the authority whenever it reads; the fixed-point slot is
        // stale on current clients (it sat ~2000 units away from the player).
        if (livePublished || UiLivePositionFresh())
            return true;

        // On current clients this slot freezes at the load position. Once the live
        // position tracker has locked onto a moving copy of the player position, it is
        // the authority; publishing the frozen slot as well would make the map jump
        // back and forth between the two.
        {
            const DWORD trackerTick = g_liveTrackerPublishTick.load();
            if (trackerTick != 0 && GetTickCount() - trackerTick < 1500)
                return false;
        }

        CapturedPlayerPosition position{};
        position.x = x;
        position.y = y;
        position.z = z;
        position.source = reinterpret_cast<uintptr_t>(state);
        position.offset = static_cast<std::uint32_t>(UI_STATE_PLAYER_POSITION_MAY24);
        position.channel = 5;
        position.lastUpdateTick = GetTickCount();
        position.valid = true;
        PublishPlayerPosition(position);
        return true;
    }

    bool TryCapturePlayerPositionFromUiRecord(const UiRenderSetupIterationRecord& record)
    {
        // Another player's UI state: none of its blocks describe us.
        if (record.state == nullptr || !AcceptUiState(record.state))
            return false;

        // Exact known offset beats heuristic block scans (which can latch onto static
        // decoys like the (0, 0, 256.079) block at the old camera offset).
        if (TryPublishExactUiStatePosition(record.state))
            return true;

        PlayerPositionCandidate best{};
        TryCapturePlayerPositionFromBlock(reinterpret_cast<uintptr_t>(record.source), 1, 0x340, best);
        TryCapturePlayerPositionFromBlock(reinterpret_cast<uintptr_t>(record.local30), 2, 0x40, best);
        TryCapturePlayerPositionFromBlock(reinterpret_cast<uintptr_t>(record.state + UI_STATE_CAMERA_BLOCK_OLD), 3, 0x40, best);
        TryCapturePlayerPositionFromBlock(reinterpret_cast<uintptr_t>(record.state + UI_STATE_CAMERA_BLOCK_NEW), 4, 0x40, best);
        if (!best.valid)
            return false;

        return PublishBestPlayerPosition(best);
    }

    bool IsLikelyRuntimePointer(uintptr_t value)
    {
        return value > 0x10000 && value < 0x0000800000000000;
    }

    bool PublishBestPlayerPosition(const PlayerPositionCandidate& best)
    {
        if (!best.valid)
            return false;

        CapturedPlayerPosition position{};
        position.x = best.x;
        position.y = best.y;
        position.z = best.z;
        position.source = best.source;
        position.offset = best.offset;
        position.channel = best.channel;
        position.lastUpdateTick = GetTickCount();
        position.valid = true;
        PublishPlayerPosition(position);
        return true;
    }

    // ---- BEGIN LIVE POSITION TRACKER ----
    // The UI-state position slot (UI_STATE_PLAYER_POSITION_MAY24) is written when the
    // world loads (and on teleports) and then stays frozen on current clients, so it is
    // only good as a starting point. This tracker snapshots every heap triple near that
    // starting point (int64 32.32 fixed, double and float), watches the snapshot for
    // values that change, and follows the group of copies that moves like the player.
    // Copies that sat exactly on the load position when the world came up are preferred:
    // those are the player's own transforms, not a creature walking nearby.
    enum : std::uint8_t
    {
        LIVE_FMT_I64 = 0,
        LIVE_FMT_F64 = 1,
        LIVE_FMT_F32 = 2,
    };

    struct LiveTrackerCandidate
    {
        uintptr_t address = 0;
        std::uint64_t raw[3] = {};
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float initialDistance = 0.0f;
        DWORD lastMoveTick = 0;
        std::uint32_t moveCount = 0;
        std::uint8_t format = LIVE_FMT_I64;
        bool alive = true;
        bool inUiState = false;
    };

    struct LiveScanBounds
    {
        float loX = 0.0f;
        float hiX = 0.0f;
        float loY = 0.0f;
        float hiY = 0.0f;
        float loZ = 0.0f;
        float hiZ = 0.0f;
    };

    std::atomic<bool> g_liveTrackerRunning{ false };
    std::atomic<DWORD> g_liveTrackerLastStartTick{ 0 };

    std::size_t LiveFormatSize(std::uint8_t format)
    {
        return format == LIVE_FMT_F32 ? sizeof(float) * 3 : sizeof(std::int64_t) * 3;
    }

    const char* LiveFormatName(std::uint8_t format)
    {
        switch (format)
        {
        case LIVE_FMT_I64: return "i64";
        case LIVE_FMT_F64: return "f64";
        default: return "f32";
        }
    }

    // Decodes one triple from raw bytes. Returns false for NaN/inf.
    bool DecodeLiveTriple(const std::uint8_t* bytes, std::uint8_t format, std::uint64_t raw[3], float& x, float& y, float& z)
    {
        if (format == LIVE_FMT_F32)
        {
            std::uint32_t r[3] = {};
            std::memcpy(r, bytes, sizeof(r));
            float v[3] = {};
            std::memcpy(v, bytes, sizeof(v));
            if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2]))
                return false;
            raw[0] = r[0];
            raw[1] = r[1];
            raw[2] = r[2];
            x = v[0];
            y = v[1];
            z = v[2];
            return true;
        }

        std::memcpy(raw, bytes, sizeof(std::uint64_t) * 3);
        if (format == LIVE_FMT_F64)
        {
            double v[3] = {};
            std::memcpy(v, bytes, sizeof(v));
            if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2]))
                return false;
            x = static_cast<float>(v[0]);
            y = static_cast<float>(v[1]);
            z = static_cast<float>(v[2]);
            return true;
        }

        x = FixedToWorld(static_cast<std::int64_t>(raw[0]));
        y = FixedToWorld(static_cast<std::int64_t>(raw[1]));
        z = FixedToWorld(static_cast<std::int64_t>(raw[2]));
        return true;
    }

    bool InLiveBounds(const LiveScanBounds& b, float x, float y, float z)
    {
        return x >= b.loX && x <= b.hiX && z >= b.loZ && z <= b.hiZ && y >= b.loY && y <= b.hiY;
    }

    // Scans [base, base + size) for triples inside the bounds (all three formats).
    void ScanRangeForLiveTriples(
        uintptr_t base,
        std::size_t size,
        const LiveScanBounds& bounds,
        float refX,
        float refY,
        float refZ,
        bool inUiState,
        std::vector<std::uint8_t>& buffer,
        std::vector<LiveTrackerCandidate>& out,
        std::size_t maxCandidates,
        std::uint64_t& scannedBytes)
    {
        constexpr std::size_t CHUNK = 0x10000;
        constexpr std::size_t TRIPLE = sizeof(std::int64_t) * 3;
        const std::int64_t loXi = WorldToFixed(bounds.loX);
        const std::int64_t hiXi = WorldToFixed(bounds.hiX);
        const double loXd = bounds.loX;
        const double hiXd = bounds.hiX;

        std::size_t pos = 0;
        while (pos < size && out.size() < maxCandidates)
        {
            const std::size_t chunk = MinValue<std::size_t>(CHUNK, size - pos);
            if (chunk >= TRIPLE && SafeRead(base + pos, buffer.data(), chunk))
            {
                scannedBytes += chunk;
                const std::uint8_t* data = buffer.data();
                for (std::size_t offset = 0; offset + sizeof(float) * 3 <= chunk && out.size() < maxCandidates; offset += sizeof(float))
                {
                    std::uint8_t formats[3] = {};
                    std::size_t formatCount = 0;

                    if ((offset & 7) == 0 && offset + TRIPLE <= chunk)
                    {
                        std::int64_t xi = 0;
                        std::memcpy(&xi, data + offset, sizeof(xi));
                        if (xi >= loXi && xi <= hiXi)
                            formats[formatCount++] = LIVE_FMT_I64;

                        double xd = 0.0;
                        std::memcpy(&xd, data + offset, sizeof(xd));
                        if (xd >= loXd && xd <= hiXd)
                            formats[formatCount++] = LIVE_FMT_F64;
                    }

                    float xf = 0.0f;
                    std::memcpy(&xf, data + offset, sizeof(xf));
                    if (xf >= bounds.loX && xf <= bounds.hiX)
                        formats[formatCount++] = LIVE_FMT_F32;

                    for (std::size_t f = 0; f < formatCount; ++f)
                    {
                        LiveTrackerCandidate candidate{};
                        if (!DecodeLiveTriple(data + offset, formats[f], candidate.raw, candidate.x, candidate.y, candidate.z))
                            continue;
                        if (!InLiveBounds(bounds, candidate.x, candidate.y, candidate.z))
                            continue;
                        // Reject degenerate blocks (all three the same value).
                        if (candidate.raw[0] == candidate.raw[1] && candidate.raw[1] == candidate.raw[2])
                            continue;

                        candidate.address = base + pos + offset;
                        candidate.format = formats[f];
                        candidate.inUiState = inUiState;
                        const float dx = candidate.x - refX;
                        const float dy = candidate.y - refY;
                        const float dz = candidate.z - refZ;
                        candidate.initialDistance = std::sqrt(dx * dx + dy * dy + dz * dz);
                        out.push_back(candidate);
                    }
                }
            }

            pos += chunk >= CHUNK ? CHUNK - TRIPLE : chunk;
        }
    }

    std::vector<LiveTrackerCandidate> ScanForPositionCopies(
        float refX,
        float refY,
        float refZ,
        float radius,
        uintptr_t uiState,
        std::size_t maxCandidates,
        std::uint64_t& scannedBytes,
        bool& truncated)
    {
        LiveScanBounds nearBounds{};
        nearBounds.loX = refX - radius;
        nearBounds.hiX = refX + radius;
        nearBounds.loZ = refZ - radius;
        nearBounds.hiZ = refZ + radius;
        nearBounds.loY = refY - radius;
        nearBounds.hiY = refY + radius;

        std::vector<LiveTrackerCandidate> found;
        found.reserve(65536);
        std::vector<std::uint8_t> buffer(0x10000);
        scannedBytes = 0;

        // The UI state block first, with world-sized bounds: if the position slot just
        // moved inside that structure on a newer client, it shows up here.
        if (uiState != 0)
        {
            LiveScanBounds world{};
            world.loX = 1.0f;
            world.hiX = REAL_MAP_WORLD_SIZE + 64.0f;
            world.loZ = 1.0f;
            world.hiZ = REAL_MAP_WORLD_SIZE + 64.0f;
            world.loY = -2000.0f;
            world.hiY = 5000.0f;
            ScanRangeForLiveTriples(uiState, 0x30000, world, refX, refY, refZ, true, buffer, found, maxCandidates, scannedBytes);
        }

        MEMORY_BASIC_INFORMATION mbi{};
        uintptr_t address = 0x10000;
        while (address < 0x00007FFFFFFF0000ULL && found.size() < maxCandidates && MinimapRuntimeActive())
        {
            if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)) == 0)
                break;

            const uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            const std::size_t regionSize = mbi.RegionSize;
            const bool scannable =
                mbi.State == MEM_COMMIT &&
                (mbi.Type == MEM_PRIVATE || mbi.Type == MEM_MAPPED) &&
                (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_EXECUTE_READWRITE) &&
                regionSize <= 0x40000000;

            if (scannable)
                ScanRangeForLiveTriples(regionBase, regionSize, nearBounds, refX, refY, refZ, false, buffer, found, maxCandidates, scannedBytes);

            address = regionBase + regionSize;
        }

        truncated = found.size() >= maxCandidates;
        std::sort(found.begin(), found.end(), [](const LiveTrackerCandidate& a, const LiveTrackerCandidate& b) {
            return a.address < b.address;
        });
        return found;
    }

    // Re-reads every live candidate. Candidates are sorted by address, so nearby ones
    // are read with a single copy. Returns the number still alive.
    std::size_t SampleLiveCandidates(
        std::vector<LiveTrackerCandidate>& candidates,
        DWORD sampleTick,
        float maxStep,
        uintptr_t adopted,
        bool& adoptedJumped,
        std::vector<std::uint8_t>& buffer)
    {
        constexpr std::size_t SPAN = 0x4000;
        std::size_t alive = 0;
        std::size_t i = 0;
        while (i < candidates.size())
        {
            if (!candidates[i].alive)
            {
                ++i;
                continue;
            }

            // Group [i, j) whose bytes fit in one span.
            const uintptr_t spanBase = candidates[i].address;
            std::size_t j = i;
            uintptr_t spanEnd = spanBase;
            while (j < candidates.size())
            {
                const uintptr_t end = candidates[j].address + LiveFormatSize(candidates[j].format);
                if (end - spanBase > SPAN)
                    break;
                spanEnd = MaxValue(spanEnd, end);
                ++j;
            }
            if (j == i)
            {
                spanEnd = spanBase + LiveFormatSize(candidates[i].format);
                j = i + 1;
            }

            const std::size_t spanSize = static_cast<std::size_t>(spanEnd - spanBase);
            const bool spanOk = SafeRead(spanBase, buffer.data(), spanSize);

            for (std::size_t k = i; k < j; ++k)
            {
                LiveTrackerCandidate& candidate = candidates[k];
                if (!candidate.alive)
                    continue;

                std::uint8_t local[24] = {};
                const std::uint8_t* bytes = nullptr;
                if (spanOk)
                {
                    bytes = buffer.data() + (candidate.address - spanBase);
                }
                else if (SafeRead(candidate.address, local, LiveFormatSize(candidate.format)))
                {
                    bytes = local;
                }

                std::uint64_t raw[3] = {};
                float x = 0.0f;
                float y = 0.0f;
                float z = 0.0f;
                if (bytes == nullptr ||
                    !DecodeLiveTriple(bytes, candidate.format, raw, x, y, z) ||
                    !IsOnMapWorldPosition(x, y, z))
                {
                    candidate.alive = false;
                    continue;
                }

                if (raw[0] != candidate.raw[0] || raw[1] != candidate.raw[1] || raw[2] != candidate.raw[2])
                {
                    const float dx = x - candidate.x;
                    const float dy = y - candidate.y;
                    const float dz = z - candidate.z;
                    const float step = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (step > maxStep)
                    {
                        // Teleport / fast travel, or the memory got reused.
                        if (candidate.address == adopted)
                            adoptedJumped = true;
                        candidate.alive = false;
                        continue;
                    }

                    std::memcpy(candidate.raw, raw, sizeof(raw));
                    candidate.x = x;
                    candidate.y = y;
                    candidate.z = z;
                    // Sub-centimetre jitter is not movement.
                    if (step > 0.01f)
                    {
                        candidate.lastMoveTick = sampleTick;
                        ++candidate.moveCount;
                    }
                }

                ++alive;
            }

            i = j;
        }

        return alive;
    }

    void RunLivePositionTracker()
    {
        constexpr std::size_t MAX_SNAPSHOT = 600000;
        constexpr std::size_t TRACK_LIMIT = 20000;
        constexpr float MAX_STEP = 40.0f;          // world units per sample
        constexpr float CLUSTER_CELL = 24.0f;
        constexpr float ON_LOAD_POSITION = 4.0f;   // "was the player" radius at scan time

        std::vector<LiveTrackerCandidate> candidates;
        std::vector<std::uint8_t> sampleBuffer(0x4000 + 64);
        uintptr_t adopted = 0;
        DWORD lastScanTick = 0;
        DWORD lastSelectTick = 0;
        DWORD lastMoverTick = GetTickCount();
        DWORD firstChangeTick = 0;
        DWORD lastStatusLogTick = 0;
        std::uint32_t scanSerial = 0;
        std::uint32_t scanCount = 0;
        float scanRadius = 320.0f;
        bool loggedMovers = false;
        bool forceExactReference = true;

        while (MinimapRuntimeActive())
        {
            if (UiLivePositionFresh())
            {
                Log("[Minimap] live position tracker idle: ui_state live slot is active");
                break;
            }

            const DWORD now = GetTickCount();
            const std::uint32_t exactSerial = g_exactSlotSerial.load();

            // (Re)scan when there is nothing to watch, when the load/teleport slot moved
            // while we are not locked on, or nothing has moved for a long time.
            const bool exactMoved = exactSerial != scanSerial && adopted == 0;
            const bool needScan = candidates.empty() ||
                exactMoved ||
                (adopted == 0 && now - lastMoverTick > 20000 && now - lastScanTick > 20000);
            if (needScan)
            {
                float refX = 0.0f;
                float refY = 0.0f;
                float refZ = 0.0f;
                const char* refName = "none";
                if ((forceExactReference || exactMoved) && g_exactSlotValid.load())
                {
                    std::lock_guard<std::mutex> lock(g_playerPositionMutex);
                    refX = FixedToWorld(g_exactSlotRaw.x);
                    refY = FixedToWorld(g_exactSlotRaw.y);
                    refZ = FixedToWorld(g_exactSlotRaw.z);
                    refName = "ui_state_slot";
                }
                else
                {
                    std::lock_guard<std::mutex> lock(g_playerPositionMutex);
                    if (g_playerPosition.valid)
                    {
                        refX = FixedToWorld(g_playerPosition.x);
                        refY = FixedToWorld(g_playerPosition.y);
                        refZ = FixedToWorld(g_playerPosition.z);
                        refName = PlayerPositionChannelName(g_playerPosition.channel);
                    }
                }

                if (std::strcmp(refName, "none") == 0)
                {
                    Sleep(250);
                    continue;
                }

                // Right after load the slot flips from a placeholder to the real spawn
                // within a few seconds; give it a moment before the first scan.
                if (scanCount == 0 && GetTickCount() - g_exactSlotChangeTick.load() < 5000)
                {
                    Sleep(250);
                    continue;
                }

                // A stale-reference retry searches wider.
                if (!exactMoved && scanCount != 0 && candidates.empty() == false)
                    scanRadius = MinValue(scanRadius * 2.0f, 1024.0f);
                if (exactMoved)
                    scanRadius = 320.0f;

                const DWORD scanStart = GetTickCount();
                std::uint64_t scannedBytes = 0;
                bool truncated = false;
                scanSerial = exactSerial;
                candidates = ScanForPositionCopies(
                    refX, refY, refZ, scanRadius,
                    g_uiStateAddress.load(),
                    MAX_SNAPSHOT, scannedBytes, truncated);
                ++scanCount;
                lastScanTick = GetTickCount();
                lastMoverTick = lastScanTick;
                firstChangeTick = 0;
                adopted = 0;
                loggedMovers = false;
                forceExactReference = false;

                std::size_t counts[3] = {};
                std::size_t onLoad = 0;
                std::size_t ui = 0;
                for (const LiveTrackerCandidate& c : candidates)
                {
                    ++counts[c.format];
                    if (c.initialDistance < ON_LOAD_POSITION)
                        ++onLoad;
                    if (c.inUiState)
                        ++ui;
                }

                std::ostringstream oss;
                oss << "[Minimap] live position scan #" << scanCount
                    << " | reference=(" << refX << ", " << refY << ", " << refZ << ")"
                    << " | reference_channel=" << refName
                    << " | radius=" << scanRadius
                    << " | scanned=" << (scannedBytes / (1024 * 1024)) << "MB"
                    << " | ms=" << (lastScanTick - scanStart)
                    << " | candidates=" << candidates.size()
                    << " (i64=" << counts[LIVE_FMT_I64] << " f64=" << counts[LIVE_FMT_F64] << " f32=" << counts[LIVE_FMT_F32] << ")"
                    << " | on_load_position=" << onLoad
                    << " | ui_state=" << ui
                    << (truncated ? " | TRUNCATED" : "");
                Log(oss.str());

                if (truncated && scanRadius > 80.0f)
                    scanRadius *= 0.5f;

                if (candidates.empty())
                {
                    Sleep(2000);
                    continue;
                }
            }

            const DWORD sampleTick = GetTickCount();
            bool adoptedJumped = false;
            const std::size_t alive = SampleLiveCandidates(candidates, sampleTick, MAX_STEP, adopted, adoptedJumped, sampleBuffer);

            if (adoptedJumped)
            {
                Log("[Minimap] live position tracker lost its lock (jump); rescanning");
                candidates.clear();
                adopted = 0;
                scanRadius = 320.0f;
                continue;
            }

            if (alive == 0)
            {
                candidates.clear();
                adopted = 0;
                continue;
            }

            LiveTrackerCandidate* adoptedCandidate = nullptr;

            if (sampleTick - lastSelectTick >= 500)
            {
                lastSelectTick = sampleTick;

                std::size_t changed = 0;
                std::size_t onLoadAlive = 0;
                std::size_t onLoadMovers = 0;
                std::size_t movers = 0;
                for (const LiveTrackerCandidate& c : candidates)
                {
                    if (!c.alive)
                        continue;
                    const bool isOnLoad = c.initialDistance < ON_LOAD_POSITION;
                    if (isOnLoad)
                        ++onLoadAlive;
                    if (c.moveCount != 0)
                        ++changed;
                    if (c.moveCount >= 4 && sampleTick - c.lastMoveTick <= 1500)
                    {
                        ++movers;
                        if (isOnLoad)
                            ++onLoadMovers;
                    }
                }

                if (changed != 0 && firstChangeTick == 0)
                    firstChangeTick = sampleTick;

                // Shrink a big snapshot to what actually changes (plus the copies that
                // sat on the load position, which may simply not have moved yet).
                if (candidates.size() > TRACK_LIMIT && firstChangeTick != 0 && sampleTick - firstChangeTick >= 1500)
                {
                    const std::size_t before = candidates.size();
                    candidates.erase(
                        std::remove_if(candidates.begin(), candidates.end(), [&](const LiveTrackerCandidate& c) {
                            return !c.alive || (c.moveCount == 0 && c.initialDistance >= ON_LOAD_POSITION && !c.inUiState);
                        }),
                        candidates.end());
                    if (candidates.size() > TRACK_LIMIT)
                    {
                        // Keep the most active ones.
                        std::stable_sort(candidates.begin(), candidates.end(), [&](const LiveTrackerCandidate& a, const LiveTrackerCandidate& b) {
                            const bool aKeep = a.initialDistance < ON_LOAD_POSITION || a.inUiState;
                            const bool bKeep = b.initialDistance < ON_LOAD_POSITION || b.inUiState;
                            if (aKeep != bKeep)
                                return aKeep;
                            return a.moveCount > b.moveCount;
                        });
                        candidates.resize(TRACK_LIMIT);
                        std::sort(candidates.begin(), candidates.end(), [](const LiveTrackerCandidate& a, const LiveTrackerCandidate& b) {
                            return a.address < b.address;
                        });
                    }

                    std::ostringstream oss;
                    oss << "[Minimap] live position snapshot narrowed | before=" << before
                        << " | after=" << candidates.size()
                        << " | changed=" << changed
                        << " | on_load_position=" << onLoadAlive;
                    Log(oss.str());
                }

                // Pick the dominant moving group. Groups containing copies that were on
                // the load position win over everything else.
                struct Cell
                {
                    std::int64_t key = 0;
                    std::uint32_t members = 0;
                    std::uint32_t onLoadMembers = 0;
                    std::uint32_t bestScore = 0;
                    uintptr_t best = 0;
                    bool hasAdopted = false;
                };
                std::vector<Cell> cells;
                for (const LiveTrackerCandidate& c : candidates)
                {
                    if (!c.alive || c.moveCount < 4 || sampleTick - c.lastMoveTick > 1500)
                        continue;

                    const std::int64_t cellX = static_cast<std::int64_t>(std::floor(c.x / CLUSTER_CELL));
                    const std::int64_t cellZ = static_cast<std::int64_t>(std::floor(c.z / CLUSTER_CELL));
                    const std::int64_t key = cellX * 100000 + cellZ;
                    Cell* cell = nullptr;
                    for (Cell& existing : cells)
                    {
                        if (existing.key == key)
                        {
                            cell = &existing;
                            break;
                        }
                    }
                    if (cell == nullptr)
                    {
                        cells.push_back(Cell{});
                        cell = &cells.back();
                        cell->key = key;
                    }

                    ++cell->members;
                    const bool isOnLoad = c.initialDistance < ON_LOAD_POSITION;
                    if (isOnLoad)
                        ++cell->onLoadMembers;
                    // Prefer exact fixed-point copies, then doubles, then floats.
                    const std::uint32_t score = c.moveCount * 4 +
                        (c.format == LIVE_FMT_I64 ? 3u : c.format == LIVE_FMT_F64 ? 2u : 1u) +
                        (isOnLoad ? 100000u : 0u);
                    if (score > cell->bestScore)
                    {
                        cell->bestScore = score;
                        cell->best = c.address;
                    }
                    if (c.address == adopted)
                        cell->hasAdopted = true;
                }

                if (movers != 0)
                {
                    lastMoverTick = sampleTick;

                    if (!loggedMovers)
                    {
                        loggedMovers = true;
                        std::vector<const LiveTrackerCandidate*> top;
                        for (const LiveTrackerCandidate& c : candidates)
                        {
                            if (c.alive && c.moveCount >= 4)
                                top.push_back(&c);
                        }
                        std::sort(top.begin(), top.end(), [](const LiveTrackerCandidate* a, const LiveTrackerCandidate* b) {
                            return a->initialDistance < b->initialDistance;
                        });
                        const uintptr_t ui = g_uiStateAddress.load();
                        for (std::size_t t = 0; t < top.size() && t < 8; ++t)
                        {
                            const LiveTrackerCandidate& c = *top[t];
                            std::ostringstream oss;
                            oss << "[Minimap] live position mover | address=" << Hex(c.address)
                                << " | format=" << LiveFormatName(c.format)
                                << " | world=(" << c.x << ", " << c.y << ", " << c.z << ")"
                                << " | moves=" << c.moveCount
                                << " | start_distance=" << c.initialDistance;
                            if (ui != 0 && c.address >= ui && c.address < ui + 0x30000)
                                oss << " | ui_state_offset=" << Hex(c.address - ui);
                            Log(oss.str());
                        }
                    }

                    // While copies from the load position are still being watched, only
                    // they may be adopted for a while: something else moving nearby is a
                    // creature, not the player standing still.
                    const bool onLoadOnly = onLoadAlive != 0 && sampleTick - lastScanTick < 10000;
                    const Cell* biggest = nullptr;
                    for (const Cell& cell : cells)
                    {
                        if (onLoadOnly && cell.onLoadMembers == 0)
                            continue;
                        if (biggest == nullptr)
                        {
                            biggest = &cell;
                            continue;
                        }
                        const bool cellOnLoad = cell.onLoadMembers != 0;
                        const bool bestOnLoad = biggest->onLoadMembers != 0;
                        if (cellOnLoad != bestOnLoad)
                        {
                            if (cellOnLoad)
                                biggest = &cell;
                            continue;
                        }
                        if (cell.members > biggest->members ||
                            (cell.members == biggest->members && cell.hasAdopted))
                        {
                            biggest = &cell;
                        }
                    }

                    if (biggest != nullptr && !biggest->hasAdopted && biggest->best != 0 && biggest->best != adopted)
                    {
                        const bool firstLock = adopted == 0;
                        adopted = biggest->best;
                        std::ostringstream oss;
                        oss << "[Minimap] live position tracker " << (firstLock ? "locked" : "switched")
                            << " | address=" << Hex(adopted)
                            << " | movers=" << movers
                            << " | group=" << biggest->members
                            << " | group_on_load=" << biggest->onLoadMembers
                            << " | groups=" << cells.size();
                        for (const LiveTrackerCandidate& c : candidates)
                        {
                            if (c.address == adopted)
                            {
                                oss << " | format=" << LiveFormatName(c.format)
                                    << " | world=(" << c.x << ", " << c.y << ", " << c.z << ")";
                                break;
                            }
                        }
                        Log(oss.str());
                    }
                }

                if (sampleTick - lastStatusLogTick >= 5000 && g_debugLoggingEnabled.load())
                {
                    lastStatusLogTick = sampleTick;
                    std::ostringstream oss;
                    oss << "[Minimap] live position tracker status"
                        << " | candidates=" << candidates.size()
                        << " | alive=" << alive
                        << " | changed=" << changed
                        << " | movers=" << movers
                        << " | on_load=" << onLoadAlive
                        << " | on_load_movers=" << onLoadMovers
                        << " | groups=" << cells.size()
                        << " | locked=" << (adopted != 0 ? "yes" : "no");
                    Log(oss.str());
                }
            }

            if (adopted != 0)
            {
                for (LiveTrackerCandidate& c : candidates)
                {
                    if (c.address == adopted)
                    {
                        adoptedCandidate = c.alive ? &c : nullptr;
                        break;
                    }
                }
                if (adoptedCandidate == nullptr)
                    adopted = 0;
            }

            if (adoptedCandidate != nullptr)
            {
                CapturedPlayerPosition position{};
                if (adoptedCandidate->format == LIVE_FMT_I64)
                {
                    position.x = static_cast<std::int64_t>(adoptedCandidate->raw[0]);
                    position.y = static_cast<std::int64_t>(adoptedCandidate->raw[1]);
                    position.z = static_cast<std::int64_t>(adoptedCandidate->raw[2]);
                }
                else
                {
                    position.x = WorldToFixed(adoptedCandidate->x);
                    position.y = WorldToFixed(adoptedCandidate->y);
                    position.z = WorldToFixed(adoptedCandidate->z);
                }
                position.source = adoptedCandidate->address;
                position.offset = 0;
                position.channel = 6;
                position.lastUpdateTick = GetTickCount();
                position.valid = true;
                g_liveTrackerPublishTick.store(position.lastUpdateTick);
                PublishPlayerPosition(position);
            }

            Sleep(candidates.size() > TRACK_LIMIT ? 150 : 40);
        }

        g_liveTrackerPublishTick.store(0);
    }

    void MaybeStartLivePositionTracker()
    {
        if (!MinimapRuntimeActive() || UiLivePositionFresh())
            return;

        const DWORD now = GetTickCount();
        const DWORD last = g_liveTrackerLastStartTick.load();
        if (last != 0 && now - last < 2000)
            return;

        if (g_liveTrackerRunning.exchange(true))
            return;

        g_liveTrackerLastStartTick.store(now);
        std::thread([]()
        {
            BackgroundThreadScope scope;
            RunLivePositionTracker();
            g_liveTrackerRunning.store(false);
        }).detach();
    }
    // ---- END LIVE POSITION TRACKER ----

    bool SameMarkerPosition(const CapturedNearbyMarker& left, const CapturedNearbyMarker& right)
    {
        if (!left.hasWorldPosition || !right.hasWorldPosition)
            return std::memcmp(left.raw, right.raw, sizeof(left.raw)) == 0;

        const float dx = FixedToWorld(left.x) - FixedToWorld(right.x);
        const float dz = FixedToWorld(left.z) - FixedToWorld(right.z);
        return std::abs(dx) < 2.0f && std::abs(dz) < 2.0f;
    }

    void PushNearbyMarkerUnique(std::vector<CapturedNearbyMarker>& markers, const CapturedNearbyMarker& marker)
    {
        for (const CapturedNearbyMarker& existing : markers)
        {
            if (SameMarkerPosition(existing, marker))
                return;
        }

        markers.push_back(marker);
    }

    bool TryReadNearbyMarker(uintptr_t entryAddress, std::uint32_t kind, CapturedNearbyMarker& marker)
    {
        marker = {};
        marker.kind = kind;
        if (!SafeRead(entryAddress, marker.raw, sizeof(marker.raw)))
            return false;

        marker.hasWorldPosition = TryDecodeFixedWorldPosition(marker.raw, marker.x, marker.y, marker.z);
        return true;
    }

    std::size_t CountDecodedMarkerPreview(std::uint8_t* entries, std::uint64_t count, std::size_t stride, std::uint32_t kind)
    {
        if (!IsLikelyRuntimePointer(reinterpret_cast<uintptr_t>(entries)) || count == 0 || count > NEARBY_MARKER_MAX_ENTRIES || stride < sizeof(std::uint64_t) * 3)
            return 0;

        const std::uint64_t previewCount = MinValue<std::uint64_t>(count, 5);
        std::size_t decoded = 0;
        for (std::uint64_t index = 0; index < previewCount; ++index)
        {
            CapturedNearbyMarker marker{};
            if (TryReadNearbyMarker(reinterpret_cast<uintptr_t>(entries + index * stride), kind, marker) && marker.hasWorldPosition)
                ++decoded;
        }

        return decoded;
    }

    void AppendNearbyMarkerEntriesWithStride(std::vector<CapturedNearbyMarker>& markers, std::uint8_t* entries, std::uint64_t count, std::uint32_t kind, std::size_t stride)
    {
        if (entries == nullptr || count == 0 || count > NEARBY_MARKER_MAX_ENTRIES)
            return;

        const std::uint64_t limitedCount = MinValue<std::uint64_t>(count, 96);
        markers.reserve(markers.size() + static_cast<std::size_t>(limitedCount));
        for (std::uint64_t index = 0; index < limitedCount; ++index)
        {
            CapturedNearbyMarker marker{};
            if (!TryReadNearbyMarker(reinterpret_cast<uintptr_t>(entries + index * stride), kind, marker))
                continue;

            PushNearbyMarkerUnique(markers, marker);
        }
    }

    void AppendNearbyMarkerEntries(std::vector<CapturedNearbyMarker>& markers, std::uint8_t* entries, std::uint64_t count, std::uint32_t kind)
    {
        AppendNearbyMarkerEntriesWithStride(markers, entries, count, kind, NEARBY_MARKER_ENTRY_STRIDE);
    }

    bool AppendNearbyMarkersFromCandidateArray(std::vector<CapturedNearbyMarker>& markers, std::uint8_t* entries, std::uint64_t count, std::uint32_t kind)
    {
        static const std::size_t kStrideCandidates[] = { 0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x50, 0x60, 0x80 };
        for (std::size_t stride : kStrideCandidates)
        {
            if (CountDecodedMarkerPreview(entries, count, stride, kind) < 2)
                continue;

            AppendNearbyMarkerEntriesWithStride(markers, entries, count, kind, stride);
            return true;
        }

        return false;
    }

    void AppendNearbyMarkersFromObjectScan(uintptr_t objectAddress, std::size_t scanStart, std::size_t scanBytes, std::vector<CapturedNearbyMarker>& markers, std::uint32_t kind)
    {
        if (!IsLikelyRuntimePointer(objectAddress))
            return;

        const std::size_t scanEnd = scanStart + scanBytes;
        for (std::size_t pointerOffset = scanStart; pointerOffset + sizeof(uintptr_t) <= scanEnd; pointerOffset += sizeof(uintptr_t))
        {
            std::uint8_t* entries = nullptr;
            if (!SafeReadValue(objectAddress + pointerOffset, entries) ||
                !IsLikelyRuntimePointer(reinterpret_cast<uintptr_t>(entries)))
            {
                continue;
            }

            const std::size_t countStart = pointerOffset + sizeof(uintptr_t);
            const std::size_t countEnd = MinValue(scanEnd, pointerOffset + 0x28);
            for (std::size_t countOffset = countStart; countOffset + sizeof(std::uint32_t) <= countEnd; countOffset += sizeof(std::uint32_t))
            {
                std::uint32_t count32 = 0;
                if (!SafeReadValue(objectAddress + countOffset, count32) || count32 == 0 || count32 > NEARBY_MARKER_MAX_ENTRIES)
                    continue;

                if (AppendNearbyMarkersFromCandidateArray(markers, entries, count32, kind))
                    return;
            }
        }
    }

    bool TryReadNearbyMarkerFloatEntry(uintptr_t entry, CapturedNearbyMarker& marker)
    {
        std::uint8_t buffer[0x28] = {};
        if (!SafeRead(entry, buffer, sizeof(buffer)))
            return false;

        float fx = 0.0f;
        float fy = 0.0f;
        float fz = 0.0f;
        std::memcpy(&fx, buffer + NEARBY_ENTRY_POS_FLOAT_MAY24 + 0x0, sizeof(fx));
        std::memcpy(&fy, buffer + NEARBY_ENTRY_POS_FLOAT_MAY24 + 0x4, sizeof(fy));
        std::memcpy(&fz, buffer + NEARBY_ENTRY_POS_FLOAT_MAY24 + 0x8, sizeof(fz));
        if (!std::isfinite(fx) || !std::isfinite(fy) || !std::isfinite(fz) || !IsOnMapWorldPosition(fx, fy, fz))
            return false;

        std::memcpy(marker.raw, buffer, sizeof(marker.raw));
        marker.x = WorldToFixed(fx);
        marker.y = WorldToFixed(fy);
        marker.z = WorldToFixed(fz);
        marker.kind = 1;
        marker.hasWorldPosition = true;
        return true;
    }

    void AppendNearbyMarkersFromStateMay24(std::uint8_t* state, std::vector<CapturedNearbyMarker>& markers)
    {
        std::uint8_t* entries = nullptr;
        std::uint64_t count = 0;
        if (!SafeReadValue(reinterpret_cast<uintptr_t>(state + NEARBY_ARRAY_OFFSET_MAY24), entries) ||
            !SafeReadValue(reinterpret_cast<uintptr_t>(state + NEARBY_ARRAY_OFFSET_MAY24 + 0x08), count))
        {
            return;
        }

        if (!IsLikelyRuntimePointer(reinterpret_cast<uintptr_t>(entries)) || count == 0 || count > NEARBY_MARKER_MAX_ENTRIES)
            return;

        const std::uint64_t limitedCount = MinValue<std::uint64_t>(count, 96);
        markers.reserve(markers.size() + static_cast<std::size_t>(limitedCount));
        for (std::uint64_t index = 0; index < limitedCount; ++index)
        {
            CapturedNearbyMarker marker{};
            if (!TryReadNearbyMarkerFloatEntry(reinterpret_cast<uintptr_t>(entries) + index * NEARBY_ENTRY_STRIDE_MAY24, marker))
                continue;

            PushNearbyMarkerUnique(markers, marker);
        }
    }

    bool TryResolveKnownMarkerIconKey(std::uint32_t candidate, std::uint32_t& outKey);

    // MapMarkerRegistryResource types that have no icon at all (the world map never
    // draws them). Found with tools/eml-icon-exporter.
    bool IsIconlessMapMarkerKey(std::uint32_t key)
    {
        switch (key)
        {
        case 0xD11531D2u:
        case 0x7B87EF66u:
        case 0x9786A9F3u:
        case 0x111ADADAu:
        case 0x454E24F2u:
        case 0xD1B00909u:
            return true;
        default:
            return false;
        }
    }

    struct CapturedMasterMarker
    {
        std::int64_t x = 0;
        std::int64_t y = 0;
        std::int64_t z = 0;
        std::uint32_t key = 0;
        std::uint32_t entity = 0;   // entry+0x08
        std::uint8_t source = 0;    // index into MARKER_ARRAY_OFFSETS_SEP26
        bool moving = false;    // moved within MASTER_MARKER_MOVING_MS
    };

    // Moving-marker tracker: markers are matched frame to frame by key + proximity.
    // Remote players are the moving markers whose key has no map icon.
    constexpr DWORD MASTER_MARKER_MOVING_MS = 20000;
    constexpr DWORD MASTER_MARKER_TRACK_FORGET_MS = 10000;
    constexpr float MASTER_MARKER_TRACK_MATCH = 40.0f;

    struct MasterMarkerTrack
    {
        std::uint32_t key = 0;
        std::uint8_t source = 0;
        float x = 0.0f;
        float z = 0.0f;
        DWORD lastSeenTick = 0;
        DWORD lastMoveTick = 0;
    };

    std::vector<MasterMarkerTrack> g_masterMarkerTracks;   // guarded by g_masterMarkerMutex

    std::mutex g_masterMarkerMutex;
    std::vector<CapturedMasterMarker> g_masterMarkers;

    void TryCaptureMasterMarkers(std::uint8_t* state)
    {
        std::vector<CapturedMasterMarker> markers;
        std::string rawSamples;
        std::size_t sourceCounts[3] = {};
        const std::size_t sourceTotal = sizeof(MARKER_ARRAY_OFFSETS_SEP26) / sizeof(MARKER_ARRAY_OFFSETS_SEP26[0]);
        for (std::size_t source = 0; source < sourceTotal; ++source)
        {
            const std::size_t arrayOffset = MARKER_ARRAY_OFFSETS_SEP26[source];
            std::uint8_t* entries = nullptr;
            std::uint64_t count = 0;
            if (!SafeReadValue(reinterpret_cast<uintptr_t>(state + arrayOffset), entries) ||
                !SafeReadValue(reinterpret_cast<uintptr_t>(state + arrayOffset + 0x08), count))
            {
                continue;
            }

            if (!IsLikelyRuntimePointer(reinterpret_cast<uintptr_t>(entries)) || count == 0 || count > MASTER_MARKER_MAX_ENTRIES)
                continue;

            std::vector<std::uint8_t> blob(static_cast<std::size_t>(count) * MASTER_MARKER_STRIDE);
            if (!SafeRead(reinterpret_cast<uintptr_t>(entries), blob.data(), blob.size()))
                continue;

            sourceCounts[source] = static_cast<std::size_t>(count);
            for (std::uint64_t index = 0; index < count; ++index)
            {
                const std::uint8_t* entry = blob.data() + index * MASTER_MARKER_STRIDE;
                CapturedMasterMarker marker{};
                std::memcpy(&marker.x, entry + MASTER_MARKER_POS_OFFSET + 0x00, sizeof(marker.x));
                std::memcpy(&marker.y, entry + MASTER_MARKER_POS_OFFSET + 0x08, sizeof(marker.y));
                std::memcpy(&marker.z, entry + MASTER_MARKER_POS_OFFSET + 0x10, sizeof(marker.z));
                std::memcpy(&marker.key, entry + MASTER_MARKER_KEY_OFFSET, sizeof(marker.key));
                std::memcpy(&marker.entity, entry + 0x08, sizeof(marker.entity));
                marker.source = static_cast<std::uint8_t>(source);

                // Debug: raw bytes of the first entries of the two newly read arrays.
                if (source != 0 && index < 2 && g_debugLoggingEnabled.load())
                {
                    std::ostringstream raw;
                    raw << " | s" << source << "[" << index << "]=";
                    for (std::size_t b = 0; b < MASTER_MARKER_STRIDE; b += 8)
                    {
                        std::uint64_t word = 0;
                        std::memcpy(&word, entry + b, sizeof(word));
                        raw << (b ? " " : "") << std::hex << word << std::dec;
                    }
                    rawSamples += raw.str();
                }

                const float worldX = FixedToWorld(marker.x);
                const float worldZ = FixedToWorld(marker.z);
                if (std::abs(worldX) < 0.01f && std::abs(worldZ) < 0.01f)
                    continue;

                if (!IsOnMapWorldPosition(worldX, FixedToWorld(marker.y), worldZ))
                    continue;

                markers.push_back(marker);
            }
        }

        // Track movement per marker (NPCs, remote players).
        {
            const DWORD now = GetTickCount();
            std::lock_guard<std::mutex> lock(g_masterMarkerMutex);
            std::vector<bool> used(g_masterMarkerTracks.size(), false);
            for (CapturedMasterMarker& marker : markers)
            {
                const float x = FixedToWorld(marker.x);
                const float z = FixedToWorld(marker.z);
                std::size_t best = g_masterMarkerTracks.size();
                float bestDistance = MASTER_MARKER_TRACK_MATCH * MASTER_MARKER_TRACK_MATCH;
                for (std::size_t t = 0; t < g_masterMarkerTracks.size(); ++t)
                {
                    const MasterMarkerTrack& track = g_masterMarkerTracks[t];
                    if (used[t] || track.key != marker.key || track.source != marker.source)
                        continue;
                    const float dx = track.x - x;
                    const float dz = track.z - z;
                    const float distance = dx * dx + dz * dz;
                    if (distance < bestDistance)
                    {
                        bestDistance = distance;
                        best = t;
                    }
                }

                if (best == g_masterMarkerTracks.size())
                {
                    g_masterMarkerTracks.push_back({ marker.key, marker.source, x, z, now, 0 });
                    used.push_back(true);
                    continue;
                }

                MasterMarkerTrack& track = g_masterMarkerTracks[best];
                used[best] = true;
                if (bestDistance > 0.25f * 0.25f)
                {
                    track.lastMoveTick = now;
                    track.x = x;
                    track.z = z;
                }
                track.lastSeenTick = now;
                marker.moving = track.lastMoveTick != 0 && now - track.lastMoveTick < MASTER_MARKER_MOVING_MS;
            }

            g_masterMarkerTracks.erase(
                std::remove_if(g_masterMarkerTracks.begin(), g_masterMarkerTracks.end(), [now](const MasterMarkerTrack& track) {
                    return now - track.lastSeenTick > MASTER_MARKER_TRACK_FORGET_MS;
                }),
                g_masterMarkerTracks.end());
        }

        // A ping may also appear as a world-map marker; say so the moment it does.
        if (g_debugLoggingEnabled.load())
        {
            static std::atomic<DWORD> lastPingMarkerLog{ 0 };
            for (const CapturedMasterMarker& marker : markers)
            {
                if (marker.key != 0x83405288u)   // mapmarker_playerPing
                    continue;
                const DWORD now = GetTickCount();
                const DWORD last = lastPingMarkerLog.load();
                if (last != 0 && TicksSince(now, last) < 3000)
                    break;
                lastPingMarkerLog.store(now);
                std::ostringstream oss;
                oss << "[Minimap] ping marker in world map array | src=" << static_cast<int>(marker.source)
                    << " | xz=(" << FixedToWorld(marker.x) << "," << FixedToWorld(marker.z) << ")";
                Log(oss.str());
                break;
            }
        }

        // Key census (throttled): which marker keys exist, how many, and which move.
        // This is how remote-player markers are identified in a multiplayer session.
        if (g_debugLoggingEnabled.load())
        {
            static std::atomic<DWORD> lastCensusTick{ 0 };
            const DWORD now = GetTickCount();
            if (lastCensusTick.load() == 0 || now - lastCensusTick.load() >= 15000)
            {
                lastCensusTick.store(now);
                struct KeyCount
                {
                    std::uint32_t key;
                    std::uint8_t source;
                    int total;
                    int moving;
                    float x;
                    float z;
                };
                std::vector<KeyCount> counts;
                for (const CapturedMasterMarker& marker : markers)
                {
                    KeyCount* found = nullptr;
                    for (KeyCount& c : counts)
                    {
                        if (c.key == marker.key && c.source == marker.source)
                        {
                            found = &c;
                            break;
                        }
                    }
                    if (found == nullptr)
                    {
                        counts.push_back({ marker.key, marker.source, 0, 0, FixedToWorld(marker.x), FixedToWorld(marker.z) });
                        found = &counts.back();
                    }
                    ++found->total;
                    if (marker.moving)
                    {
                        ++found->moving;
                        found->x = FixedToWorld(marker.x);
                        found->z = FixedToWorld(marker.z);
                    }
                }
                std::ostringstream oss;
                oss << "[Minimap] master marker census | entries=" << markers.size()
                    << " | arrays=" << sourceCounts[0] << "/" << sourceCounts[1] << "/" << sourceCounts[2];
                for (const KeyCount& c : counts)
                {
                    oss << " | " << (c.source != 0 ? (c.source == 1 ? "s1:" : "s2:") : "") << Hex(c.key) << " n=" << c.total;
                    if (c.moving != 0)
                        oss << " moving=" << c.moving << "@(" << c.x << "," << c.z << ")";
                    if (c.source != 0 && c.moving == 0)
                        oss << "@(" << c.x << "," << c.z << ")";
                }
                oss << rawSamples;
                Log(oss.str());

                // Where the icon-less markers (NPC candidates) are, once a minute.
                static std::atomic<DWORD> lastNpcDumpTick{ 0 };
                if (lastNpcDumpTick.load() == 0 || now - lastNpcDumpTick.load() >= 60000)
                {
                    lastNpcDumpTick.store(now);
                    std::ostringstream npc;
                    npc << "[Minimap] npc-like markers";
                    int logged = 0;
                    for (const CapturedMasterMarker& marker : markers)
                    {
                        if (marker.key != 0 && !IsIconlessMapMarkerKey(marker.key))
                            continue;
                        npc << " | " << Hex(marker.key) << "@(" << static_cast<int>(FixedToWorld(marker.x))
                            << "," << static_cast<int>(FixedToWorld(marker.z)) << ")"
                            << (marker.moving ? " moving" : "");
                        if (++logged >= 40)
                            break;
                    }
                    Log(npc.str());
                }
            }
        }

        // Surface unknown icon keys (throttled): needed to identify e.g. remote-player
        // markers so they can be mapped to a proper icon.
        if (g_debugLoggingEnabled.load())
        {
            static std::atomic<DWORD> lastUnknownKeyLogTick{ 0 };
            const DWORD now = GetTickCount();
            const DWORD last = lastUnknownKeyLogTick.load();
            if (last == 0 || now - last >= 15000)
            {
                std::ostringstream unknownKeys;
                int unknownCount = 0;
                for (const CapturedMasterMarker& marker : markers)
                {
                    std::uint32_t resolved = 0;
                    if (marker.key == 0 || marker.key == MASTER_KEY_FLAME_ALTAR || marker.key == MASTER_KEY_PLAYER_PING ||
                        IsIconlessMapMarkerKey(marker.key) || TryResolveKnownMarkerIconKey(marker.key, resolved))
                    {
                        continue;
                    }
                    if (unknownCount != 0)
                        unknownKeys << ",";
                    unknownKeys << Hex(marker.key) << "@(" << FixedToWorld(marker.x) << "," << FixedToWorld(marker.z) << ")";
                    if (++unknownCount >= 6)
                        break;
                }
                if (unknownCount != 0)
                {
                    lastUnknownKeyLogTick.store(now);
                    std::ostringstream oss;
                    oss << "[Minimap] master markers with unknown icon keys | " << unknownKeys.str();
                    Log(oss.str());
                }
            }
        }

        std::lock_guard<std::mutex> lock(g_masterMarkerMutex);
        g_masterMarkers = std::move(markers);
    }

    void AppendNearbyMarkersFromState(std::uint8_t* state, std::vector<CapturedNearbyMarker>& markers)
    {
        std::uint8_t* entries = nullptr;
        std::uint64_t count = 0;
        if (SafeReadValue(reinterpret_cast<uintptr_t>(state + NEARBY_MARKER_ARRAY_OFFSET), entries) &&
            SafeReadValue(reinterpret_cast<uintptr_t>(state + NEARBY_MARKER_COUNT_OFFSET), count))
        {
            AppendNearbyMarkerEntries(markers, entries, count, 1);
        }

        // NOTE: the May-24-2026 array at state+0x8E8 holds streamed world entities, not
        // map markers: feeding it into the minimap drew identical jittering icons that
        // matched nothing on the world map, and its presence suppressed the static POI
        // catalog via the runtime-match gate. Left unplugged on purpose:
        // AppendNearbyMarkersFromStateMay24(state, markers);
    }

    void AppendNearbyMarkersFromInputList(void* list, std::vector<CapturedNearbyMarker>& markers, std::uint32_t kind)
    {
        if (list == nullptr)
            return;

        std::uint8_t* entries = nullptr;
        std::uint32_t count = 0;
        const uintptr_t listAddress = reinterpret_cast<uintptr_t>(list);
        if (SafeReadValue(listAddress + INPUT_MARKER_ARRAY_OFFSET, entries) &&
            SafeReadValue(listAddress + INPUT_MARKER_COUNT_OFFSET, count))
        {
            AppendNearbyMarkerEntries(markers, entries, count, kind);
        }

        AppendNearbyMarkersFromObjectScan(reinterpret_cast<uintptr_t>(list), 0, DYNAMIC_MARKER_SCAN_BYTES, markers, kind + 10);
    }

    void PublishNearbyMarkers(std::vector<CapturedNearbyMarker>&& markers)
    {
        bool changed = false;
        std::size_t markerCount = 0;
        std::size_t decodedCount = 0;
        CapturedNearbyMarker firstDecoded{};
        bool hasFirstDecoded = false;
        {
            std::lock_guard<std::mutex> lock(g_nearbyMarkerMutex);
            changed = !SameNearbyMarkers(g_nearbyMarkers, markers);
            if (changed)
                g_nearbyMarkers = std::move(markers);

            markerCount = g_nearbyMarkers.size();
            for (const CapturedNearbyMarker& marker : g_nearbyMarkers)
            {
                if (marker.hasWorldPosition)
                {
                    ++decodedCount;
                    if (!hasFirstDecoded)
                    {
                        firstDecoded = marker;
                        hasFirstDecoded = true;
                    }
                }
            }
        }

        const DWORD now = GetTickCount();
        if (!g_debugLoggingEnabled.load())
            return;

        if (now - g_lastNearbySummaryTick < 5000)
            return;

        g_lastNearbySummaryTick = now;

        std::ostringstream oss;
        oss << "[Minimap] internal nearby marker feed: " << markerCount
            << " entries | decoded_world=" << decodedCount;
        if (hasFirstDecoded)
        {
            oss << " | first_kind=" << firstDecoded.kind
                << " | first world=("
                << FixedToWorld(firstDecoded.x) << ", "
                << FixedToWorld(firstDecoded.y) << ", "
                << FixedToWorld(firstDecoded.z) << ")"
                << " | first_raw="
                << std::hex << firstDecoded.raw[0] << ","
                << firstDecoded.raw[1] << ","
                << firstDecoded.raw[2] << ","
                << firstDecoded.raw[3] << ","
                << firstDecoded.raw[4] << std::dec;
        }
        Log(oss.str());
    }

    // iter_init/iter_next size the record from the query itself, not from the size we
    // pass: a query with more components than our struct has slots would run past it.
    // Every record therefore lives in this oversized, zeroed scratch area.
    constexpr std::size_t ITER_RECORD_SCRATCH_BYTES = 0x200;

    bool TryInitWaypointRecord(void* ctx, WaypointsUiIterationRecord& record)
    {
        __try
        {
            alignas(16) std::uint8_t scratch[ITER_RECORD_SCRATCH_BYTES] = {};
            g_iterInit(ctx, scratch, static_cast<std::uint32_t>(sizeof(record)));
            std::memcpy(&record, scratch, sizeof(record));
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool TryInitUiRenderSetupRecord(void* ctx, UiRenderSetupIterationRecord& record)
    {
        __try
        {
            // The query cursor lives in the game's iteration context (ctx+0x08), and
            // iter_init only clears the record: it never rewinds the cursor. The entity we
            // pull here would otherwise be skipped by the game's own loop right after this
            // hook, and with two matching entities its iter_next walks strides from a
            // zeroed record and faults (crash seen in local_player_ui_render_setup while
            // hosting). Put the cursor back exactly where the game expects it.
            auto* cursor = reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(ctx) + 0x08);
            const std::uint32_t savedCursor = *cursor;
            alignas(16) std::uint8_t scratch[ITER_RECORD_SCRATCH_BYTES] = {};
            auto* scratchRecord = reinterpret_cast<UiRenderSetupIterationRecord*>(scratch);
            g_iterInit(ctx, scratch, static_cast<std::uint32_t>(sizeof(record)));
            // May-24-2026 client: iter_init only prepares the cursor; fields are filled by
            // the first iter_next (matches the game's own call sequence at this hook).
            bool ok = true;
            if ((scratchRecord->source == nullptr || scratchRecord->state == nullptr) && g_iterNext != nullptr)
                ok = g_iterNext(ctx, scratch, static_cast<std::uint32_t>(sizeof(record)));
            *cursor = savedCursor;
            std::memcpy(&record, scratch, sizeof(record));
            return ok;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void PublishUiRenderSnapshot(const UiRenderSnapshot& snapshot)
    {
        if (!g_debugLoggingEnabled.load())
            return;

        const DWORD now = GetTickCount();
        if (now - g_lastUiRenderSummaryTick < 5000)
            return;

        g_lastUiRenderSummaryTick = now;

        std::ostringstream oss;
        oss << "[Minimap] internal UI render setup"
            << " | source=" << Hex(snapshot.source)
            << " | state=" << Hex(snapshot.state)
            << " | source_size=" << snapshot.sourceWidth << "x" << snapshot.sourceHeight
            << " | state_size=" << snapshot.stateWidth << "x" << snapshot.stateHeight;
        Log(oss.str());
    }

    bool TryCaptureUiRenderSetup(void* ctx)
    {
        if (g_iterInit == nullptr || ctx == nullptr)
            return false;

        UiRenderSetupIterationRecord record{};
        if (!TryInitUiRenderSetupRecord(ctx, record))
            return false;

        if (record.source == nullptr || record.state == nullptr)
            return false;

        UiRenderSnapshot snapshot{};
        snapshot.source = reinterpret_cast<uintptr_t>(record.source);
        snapshot.state = reinterpret_cast<uintptr_t>(record.state);

        SafeReadValue(reinterpret_cast<uintptr_t>(record.source + 0x30C), snapshot.sourceWidth);
        SafeReadValue(reinterpret_cast<uintptr_t>(record.source + 0x304), snapshot.sourceHeight);
        SafeReadValue(reinterpret_cast<uintptr_t>(record.state + 0x16180), snapshot.stateWidth);
        SafeReadValue(reinterpret_cast<uintptr_t>(record.state + 0x16184), snapshot.stateHeight);

        PublishUiRenderSnapshot(snapshot);
        TryCapturePlayerPositionFromUiRecord(record);
        return true;
    }

    void PublishWaypoints(std::vector<CapturedWaypoint>&& waypoints)
    {
        bool changed = false;
        std::size_t waypointCount = 0;
        CapturedWaypoint firstWaypoint{};
        bool hasFirstWaypoint = false;
        {
            std::lock_guard<std::mutex> lock(g_waypointMutex);
            changed = !SameWaypoints(g_waypoints, waypoints);
            if (changed)
                g_waypoints = std::move(waypoints);

            waypointCount = g_waypoints.size();
            if (!g_waypoints.empty())
            {
                firstWaypoint = g_waypoints.front();
                hasFirstWaypoint = true;
            }
        }

        const DWORD now = GetTickCount();
        if (!g_debugLoggingEnabled.load())
            return;

        if (!changed && now - g_lastSummaryTick < 5000)
            return;

        g_lastSummaryTick = now;

        std::ostringstream oss;
        oss << "[Minimap] internal waypoint feed: " << waypointCount << " active";
        if (hasFirstWaypoint)
        {
            oss << " | first_id=" << firstWaypoint.id
                << " | first world=("
                << FixedToWorld(firstWaypoint.x) << ", "
                << FixedToWorld(firstWaypoint.y) << ", "
                << FixedToWorld(firstWaypoint.z) << ")";
        }
        Log(oss.str());
    }

    struct WaypointBlockLayout
    {
        std::size_t arrayOffset = 0;
        std::size_t stride = 0;
        std::size_t positionOffset = 0;
    };

    std::mutex g_waypointLayoutMutex;
    WaypointBlockLayout g_waypointLayoutOverride{};
    std::atomic<DWORD> g_lastWaypointScanTick{ 0 };
    std::atomic<DWORD> g_lastWaypointFailureLogTick{ 0 };
    std::atomic<bool> g_waypointOffsetOverrideLogged{ false };
    std::atomic<int> g_waypointStateSlot{ 0 };

    constexpr std::size_t WAYPOINT_STRIDE_CANDIDATES[] = { WAYPOINT_ENTRY_STRIDE, WAYPOINT_ENTRY_STRIDE_MAY24 };
    constexpr std::size_t WAYPOINT_POSITION_CANDIDATES[] = { 0x30, 0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x38, 0x40 };

    bool TryReadWaypointBlockAt(std::uint8_t* state, std::size_t arrayOffset, std::uint8_t*& outEntries, std::uint64_t& outCount)
    {
        std::uint8_t* entries = nullptr;
        std::uint64_t count = 0;
        if (!SafeReadValue(reinterpret_cast<uintptr_t>(state + arrayOffset), entries) ||
            !SafeReadValue(reinterpret_cast<uintptr_t>(state + arrayOffset + 0x08), count))
        {
            return false;
        }

        outEntries = entries;
        outCount = count;
        return true;
    }

    bool IsOnMapWorldPosition(float x, float y, float z)
    {
        // Custom map markers always sit well inside the playable map square; this also
        // rejects static decoy blocks like (768.3, 0, ~0) that sit on an axis edge.
        return x > 1.0f && x < REAL_MAP_WORLD_SIZE + 64.0f &&
            z > 1.0f && z < REAL_MAP_WORLD_SIZE + 64.0f &&
            y > -2000.0f && y < 5000.0f;
    }

    bool WaypointEntriesMatchLayout(std::uint8_t* entries, std::uint64_t count, std::size_t stride, std::size_t positionOffset)
    {
        if (!IsLikelyRuntimePointer(reinterpret_cast<uintptr_t>(entries)) || count == 0 || count > WAYPOINT_MAX_ENTRIES)
            return false;

        const std::uint64_t samples = std::min<std::uint64_t>(count, 3);
        for (std::uint64_t index = 0; index < samples; ++index)
        {
            std::uint8_t* entry = entries + (index * stride);
            std::int64_t x = 0;
            std::int64_t y = 0;
            std::int64_t z = 0;
            if (!SafeReadValue(reinterpret_cast<uintptr_t>(entry + positionOffset + 0x00), x) ||
                !SafeReadValue(reinterpret_cast<uintptr_t>(entry + positionOffset + 0x08), y) ||
                !SafeReadValue(reinterpret_cast<uintptr_t>(entry + positionOffset + 0x10), z))
            {
                return false;
            }

            if (!IsOnMapWorldPosition(FixedToWorld(x), FixedToWorld(y), FixedToWorld(z)))
                return false;
        }

        return true;
    }

    bool TryMatchWaypointLayoutAt(std::uint8_t* state, std::size_t arrayOffset, WaypointBlockLayout& outLayout, std::uint8_t*& outEntries, std::uint64_t& outCount)
    {
        std::uint8_t* entries = nullptr;
        std::uint64_t count = 0;
        if (!TryReadWaypointBlockAt(state, arrayOffset, entries, count))
            return false;

        if (count == 0 || count > WAYPOINT_MAX_ENTRIES ||
            !IsLikelyRuntimePointer(reinterpret_cast<uintptr_t>(entries)))
        {
            return false;
        }

        for (std::size_t stride : WAYPOINT_STRIDE_CANDIDATES)
        {
            for (std::size_t positionOffset : WAYPOINT_POSITION_CANDIDATES)
            {
                if (WaypointEntriesMatchLayout(entries, count, stride, positionOffset))
                {
                    outLayout.arrayOffset = arrayOffset;
                    outLayout.stride = stride;
                    outLayout.positionOffset = positionOffset;
                    outEntries = entries;
                    outCount = count;
                    return true;
                }
            }
        }

        return false;
    }

    bool TryScanForWaypointLayout(std::uint8_t* state, WaypointBlockLayout& outLayout, std::uint8_t*& outEntries, std::uint64_t& outCount)
    {
        constexpr std::size_t SCAN_START = WAYPOINT_ARRAY_OFFSET_MAY24 > 0x40000 ? WAYPOINT_ARRAY_OFFSET_MAY24 - 0x40000 : 0;
        constexpr std::size_t SCAN_END = WAYPOINT_ARRAY_OFFSET + 0x40000;
        constexpr std::size_t CHUNK_BYTES = 0x1000;

        std::uint8_t chunk[CHUNK_BYTES + sizeof(std::uint64_t)];
        for (std::size_t base = SCAN_START; base < SCAN_END; base += CHUNK_BYTES)
        {
            // Read one extra qword so a (pointer, count) pair straddling the chunk edge is still seen.
            if (!SafeRead(reinterpret_cast<uintptr_t>(state + base), chunk, sizeof(chunk)))
                continue;

            for (std::size_t offset = 0; offset < CHUNK_BYTES; offset += sizeof(std::uint64_t))
            {
                std::uint8_t* entries = nullptr;
                std::uint64_t count = 0;
                std::memcpy(&entries, chunk + offset, sizeof(entries));
                std::memcpy(&count, chunk + offset + sizeof(std::uint64_t), sizeof(count));

                if (!IsLikelyRuntimePointer(reinterpret_cast<uintptr_t>(entries)) || count == 0 || count > WAYPOINT_MAX_ENTRIES)
                    continue;

                if (TryMatchWaypointLayoutAt(state, base + offset, outLayout, outEntries, outCount))
                    return true;
            }
        }

        return false;
    }

    bool TryResolveWaypointBlock(std::uint8_t* state, WaypointBlockLayout& outLayout, std::uint8_t*& outEntries, std::uint64_t& outCount)
    {
        std::uint8_t* entries = nullptr;
        std::uint64_t count = 0;

        // Known layouts first (offset, stride, position offset); legit-empty (count==0)
        // is accepted without further discovery.
        const WaypointBlockLayout knownLayouts[] = {
            { WAYPOINT_ARRAY_OFFSET_MAY24, WAYPOINT_ENTRY_STRIDE_MAY24, WAYPOINT_ENTRY_POSITION_MAY24 },
            { WAYPOINT_ARRAY_OFFSET, WAYPOINT_ENTRY_STRIDE, WAYPOINT_ENTRY_POSITION_OFFSET },
        };
        for (const WaypointBlockLayout& known : knownLayouts)
        {
            if (!TryReadWaypointBlockAt(state, known.arrayOffset, entries, count))
                continue;

            if (count == 0 && (entries == nullptr || IsLikelyRuntimePointer(reinterpret_cast<uintptr_t>(entries))))
            {
                outLayout = known;
                outEntries = entries;
                outCount = 0;
                return true;
            }

            if (WaypointEntriesMatchLayout(entries, count, known.stride, known.positionOffset))
            {
                outLayout = known;
                outEntries = entries;
                outCount = count;
                return true;
            }

            if (TryMatchWaypointLayoutAt(state, known.arrayOffset, outLayout, outEntries, outCount))
                return true;
        }

        WaypointBlockLayout cached{};
        {
            std::lock_guard<std::mutex> lock(g_waypointLayoutMutex);
            cached = g_waypointLayoutOverride;
        }

        if (cached.arrayOffset != 0 &&
            TryReadWaypointBlockAt(state, cached.arrayOffset, entries, count) &&
            WaypointEntriesMatchLayout(entries, count, cached.stride, cached.positionOffset))
        {
            outLayout = cached;
            outEntries = entries;
            outCount = count;
            return true;
        }

        const DWORD now = GetTickCount();
        const DWORD lastScan = g_lastWaypointScanTick.load();
        if (lastScan != 0 && now - lastScan < 10000)
            return false;

        g_lastWaypointScanTick.store(now);

        WaypointBlockLayout discovered{};
        if (!TryScanForWaypointLayout(state, discovered, outEntries, outCount))
            return false;

        {
            std::lock_guard<std::mutex> lock(g_waypointLayoutMutex);
            g_waypointLayoutOverride = discovered;
        }

        if (!g_waypointOffsetOverrideLogged.exchange(true))
        {
            std::ostringstream oss;
            oss << "[Minimap] waypoint array relocated by game update"
                << " | old_offset=0x" << std::hex << WAYPOINT_ARRAY_OFFSET
                << " | new_offset=0x" << discovered.arrayOffset
                << " | stride=0x" << discovered.stride
                << " | pos_offset=0x" << discovered.positionOffset << std::dec;
            Log(oss.str());
        }

        outLayout = discovered;
        return true;
    }

    std::uint8_t* ResolveWaypointsUiState(const WaypointsUiIterationRecord& record)
    {
        const int cachedSlot = g_waypointStateSlot.load();
        std::uint8_t* candidates[2] = {};
        if (cachedSlot == 2)
        {
            candidates[0] = record.stateNew;
            candidates[1] = record.state;
        }
        else
        {
            candidates[0] = record.state;
            candidates[1] = record.stateNew;
        }

        for (int index = 0; index < 2; ++index)
        {
            std::uint8_t* candidate = candidates[index];
            if (!IsLikelyRuntimePointer(reinterpret_cast<uintptr_t>(candidate)))
                continue;

            std::uint64_t probe = 0;
            if (!SafeReadValue(reinterpret_cast<uintptr_t>(candidate + UI_STATE_BIG_OBJECT_PROBE), probe))
                continue;

            const bool isNewSlot = candidate == record.stateNew;
            g_waypointStateSlot.store(isNewSlot ? 2 : 1);
            return candidate;
        }

        return nullptr;
    }

    void TryCaptureRemotePlayers(std::uint8_t* state);
    void LogRemotePlayersIfDue();

    // ---- REMOTE PLAYER PROBE (debug only) ----
    // Remote players are not in the master marker array or in the UI state. This hunt
    // searches the whole heap for copies of the local player's position, then looks for
    // objects that are "the same kind" as the object holding it:
    //   run: a short array of positions with a fixed stride that includes us (an ECS
    //        chunk that only holds players would have exactly 1 + remote players);
    //   vptr: other objects with the same exe pointer at the same relative offset.
    // Every candidate group is then watched for a few minutes to see which entries move.
    // It only logs.
    constexpr int HUNT_I64 = 0;
    constexpr int HUNT_F32 = 1;
    constexpr int HUNT_F64 = 2;

    const char* HuntFormatName(int fmt)
    {
        return fmt == HUNT_F32 ? "f32" : (fmt == HUNT_F64 ? "f64" : "i64");
    }

    std::size_t HuntTripleBytes(int fmt)
    {
        return fmt == HUNT_F32 ? 12 : 24;
    }

    bool HuntDecodeTriple(const std::uint8_t* p, int fmt, float& x, float& y, float& z)
    {
        if (fmt == HUNT_F32)
        {
            float v[3] = {};
            std::memcpy(v, p, sizeof(v));
            x = v[0];
            y = v[1];
            z = v[2];
        }
        else if (fmt == HUNT_F64)
        {
            double v[3] = {};
            std::memcpy(v, p, sizeof(v));
            if (!(std::fabs(v[0]) < 1.0e6 && std::fabs(v[1]) < 1.0e6 && std::fabs(v[2]) < 1.0e6))
                return false;
            x = static_cast<float>(v[0]);
            y = static_cast<float>(v[1]);
            z = static_cast<float>(v[2]);
        }
        else
        {
            std::int64_t v[3] = {};
            std::memcpy(v, p, sizeof(v));
            x = FixedToWorld(v[0]);
            y = FixedToWorld(v[1]);
            z = FixedToWorld(v[2]);
        }
        return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && IsOnMapWorldPosition(x, y, z);
    }

    bool HuntReadTriple(uintptr_t address, int fmt, float& x, float& y, float& z)
    {
        std::uint8_t raw[24] = {};
        if (!SafeRead(address, raw, HuntTripleBytes(fmt)))
            return false;
        return HuntDecodeTriple(raw, fmt, x, y, z);
    }

    bool HuntGetSelf(float& x, float& y, float& z)
    {
        std::lock_guard<std::mutex> lock(g_playerPositionMutex);
        if (!g_lastExactPosition.valid)
            return false;
        x = FixedToWorld(g_lastExactPosition.x);
        y = FixedToWorld(g_lastExactPosition.y);
        z = FixedToWorld(g_lastExactPosition.z);
        return true;
    }

    bool HuntIsSelf(float x, float z, float sx, float sz)
    {
        return std::fabs(x - sx) < 3.0f && std::fabs(z - sz) < 3.0f;
    }

    struct HuntGroup
    {
        std::string label;
        int fmt = HUNT_I64;
        std::vector<uintptr_t> addresses;
        std::vector<float> lastXZ;
        std::vector<std::uint32_t> moves;
        std::uint32_t samples = 0;
        std::uint32_t selfHits = 0;
    };

    std::mutex g_huntMutex;
    std::vector<HuntGroup> g_huntGroups;
    std::atomic<bool> g_huntBusy{ false };
    int g_huntRuns = 0;
    DWORD g_huntReadyTick = 0;
    DWORD g_huntLastSampleTick = 0;
    DWORD g_huntLastLogTick = 0;
    DWORD g_huntWatchUntilTick = 0;

    // Thread stacks hold short-lived copies of our position (locals, call arguments) and
    // drowned the first hunt in noise. A stack's committed part sits right above its
    // guard page inside the same allocation.
    bool HuntIsThreadStack(const MEMORY_BASIC_INFORMATION& region)
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(region.BaseAddress);
        if (base <= 0x10000)
            return false;
        MEMORY_BASIC_INFORMATION below{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(base - 1), &below, sizeof(below)) == 0)
            return false;
        return below.AllocationBase == region.AllocationBase && (below.Protect & PAGE_GUARD) != 0;
    }

    // Other entities should sit at a height near ours; this drops swapped or unrelated
    // float triples that happen to look like map coordinates.
    bool HuntPlausibleHeight(float y, float selfY)
    {
        return std::fabs(y - selfY) < 800.0f;
    }

    template <typename Fn>
    void HuntForEachRegion(Fn fn)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        uintptr_t address = 0x10000;
        while (address < 0x00007FFFFFFF0000ULL)
        {
            if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)) == 0)
                break;
            const uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            const std::size_t regionSize = mbi.RegionSize;
            if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
                (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_EXECUTE_READWRITE) &&
                regionSize <= 0x40000000 && !HuntIsThreadStack(mbi))
            {
                fn(regionBase, regionSize);
            }
            const uintptr_t next = regionBase + regionSize;
            if (next <= address)
                break;
            address = next;
        }
    }

    bool HuntRegionBounds(uintptr_t address, uintptr_t& lo, uintptr_t& hi)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT)
            return false;
        lo = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        hi = lo + mbi.RegionSize;
        return true;
    }

    struct HuntHit
    {
        uintptr_t address = 0;
        int fmt = HUNT_I64;
    };

    struct HuntKeyUse
    {
        std::int32_t delta = 0;   // pointer address minus position address
        int fmt = HUNT_I64;
        std::vector<uintptr_t> instances;
        std::uint32_t total = 0;
    };

    void RunRemotePlayerHunt(int runIndex)
    {
        constexpr std::size_t CHUNK = 0x10000;
        constexpr std::size_t OVERLAP = 0x200;
        constexpr std::size_t MAX_HITS = 4000;
        constexpr std::size_t MAX_STRIDE = 0x400;
        constexpr int MAX_RUN = 32;
        constexpr std::size_t VPTR_HITS = 800;
        constexpr std::size_t MAX_KEYS = 40000;
        constexpr std::size_t MAX_INSTANCES = 40;
        constexpr std::size_t MAX_GROUPS = 80;

        const DWORD startTick = GetTickCount();
        float sx = 0.0f, sy = 0.0f, sz = 0.0f;
        if (!HuntGetSelf(sx, sy, sz))
            return;

        std::vector<std::uint8_t> buffer(CHUNK + OVERLAP);
        const uintptr_t bufferLo = reinterpret_cast<uintptr_t>(buffer.data());
        const uintptr_t bufferHi = bufferLo + buffer.size();

        // Pass 1: every copy of our own position.
        std::vector<HuntHit> hits;
        std::uint64_t scannedBytes = 0;
        HuntForEachRegion([&](uintptr_t regionBase, std::size_t regionSize)
        {
            for (std::size_t pos = 0; pos < regionSize && hits.size() < MAX_HITS; pos += CHUNK)
            {
                const std::size_t readBytes = MinValue<std::size_t>(CHUNK + 24, regionSize - pos);
                if (readBytes < 12 || !SafeRead(regionBase + pos, buffer.data(), readBytes))
                    continue;
                scannedBytes += readBytes;
                HuntGetSelf(sx, sy, sz);
                const std::int64_t fxLo = WorldToFixed(sx - 1.5f);
                const std::int64_t fxHi = WorldToFixed(sx + 1.5f);
                const std::size_t limit = MinValue<std::size_t>(CHUNK, readBytes);
                for (std::size_t off = 0; off + 12 <= readBytes && off < limit; off += 4)
                {
                    const uintptr_t address = regionBase + pos + off;
                    if (address >= bufferLo && address < bufferHi)
                        continue;
                    const std::uint8_t* p = buffer.data() + off;
                    float f = 0.0f;
                    std::memcpy(&f, p, 4);
                    if (std::fabs(f - sx) < 1.5f)
                    {
                        float x = 0.0f, y = 0.0f, z = 0.0f;
                        if (HuntDecodeTriple(p, HUNT_F32, x, y, z) && std::fabs(z - sz) < 1.5f && std::fabs(y - sy) < 4.0f)
                            hits.push_back({ address, HUNT_F32 });
                    }
                    if ((off & 7) != 0 || off + 24 > readBytes)
                        continue;
                    std::int64_t i = 0;
                    std::memcpy(&i, p, 8);
                    if (i >= fxLo && i <= fxHi)
                    {
                        float x = 0.0f, y = 0.0f, z = 0.0f;
                        if (HuntDecodeTriple(p, HUNT_I64, x, y, z) && std::fabs(z - sz) < 1.5f && std::fabs(y - sy) < 4.0f)
                            hits.push_back({ address, HUNT_I64 });
                    }
                    double d = 0.0;
                    std::memcpy(&d, p, 8);
                    if (std::fabs(d - static_cast<double>(sx)) < 1.5)
                    {
                        float x = 0.0f, y = 0.0f, z = 0.0f;
                        if (HuntDecodeTriple(p, HUNT_F64, x, y, z) && std::fabs(z - sz) < 1.5f && std::fabs(y - sy) < 4.0f)
                            hits.push_back({ address, HUNT_F64 });
                    }
                }
            }
        });

        std::uint32_t hitsByFmt[3] = {};
        for (const HuntHit& hit : hits)
            ++hitsByFmt[hit.fmt];
        {
            std::ostringstream oss;
            oss << "[Minimap] player hunt " << runIndex << " | self=(" << sx << "," << sy << "," << sz << ")"
                << " | scanned=" << (scannedBytes >> 20) << "MB"
                << " | self_copies=" << hits.size()
                << " i64=" << hitsByFmt[HUNT_I64] << " f32=" << hitsByFmt[HUNT_F32] << " f64=" << hitsByFmt[HUNT_F64]
                << " | ms=" << (GetTickCount() - startTick);
            Log(oss.str());
        }

        std::vector<HuntGroup> groups;
        std::vector<std::string> lines;

        // Fixed-point (i64) copies are what the engine uses for entity positions: try them first.
        std::stable_partition(hits.begin(), hits.end(), [](const HuntHit& hit) { return hit.fmt == HUNT_I64; });

        // Pass 2: short fixed-stride runs of positions that include us.
        // Passes 2 and 3 only found noise in the 9/17 client logs (network pools that hold
        // every replicated entity), so they are off; pass 4 below replaces them.
        constexpr bool HUNT_LEGACY_PASSES = false;
        if (HUNT_LEGACY_PASSES)
        {
            struct RunKey { uintptr_t start; std::size_t stride; int fmt; };
            std::vector<RunKey> seen;
            std::vector<std::uint8_t> window;
            for (const HuntHit& hit : hits)
            {
                if (groups.size() >= MAX_GROUPS / 2)
                    break;
                uintptr_t regionLo = 0, regionHi = 0;
                if (!HuntRegionBounds(hit.address, regionLo, regionHi))
                    continue;
                const std::size_t span = MAX_RUN * MAX_STRIDE;
                const uintptr_t lo = (hit.address - regionLo > span) ? hit.address - span : regionLo;
                const uintptr_t hi = (regionHi - hit.address > span + 24) ? hit.address + span + 24 : regionHi;
                window.resize(static_cast<std::size_t>(hi - lo));
                if (!SafeRead(lo, window.data(), window.size()))
                    continue;

                const std::size_t tb = HuntTripleBytes(hit.fmt);
                const std::size_t step = hit.fmt == HUNT_F32 ? 4 : 8;
                const std::size_t selfOff = static_cast<std::size_t>(hit.address - lo);
                for (std::size_t stride = (tb + step - 1) / step * step; stride <= MAX_STRIDE; stride += step)
                {
                    int back = 0;
                    bool backEnded = false;
                    std::vector<float> xs;
                    std::vector<float> zs;
                    while (back < MAX_RUN)
                    {
                        const std::size_t need = static_cast<std::size_t>(back + 1) * stride;
                        if (need > selfOff)
                        {
                            backEnded = (lo == regionLo);
                            break;
                        }
                        float x = 0.0f, y = 0.0f, z = 0.0f;
                        if (!HuntDecodeTriple(window.data() + selfOff - need, hit.fmt, x, y, z) || !HuntPlausibleHeight(y, sy))
                        {
                            backEnded = true;
                            break;
                        }
                        xs.insert(xs.begin(), x);
                        zs.insert(zs.begin(), z);
                        ++back;
                    }
                    if (!backEnded)
                        continue;
                    xs.push_back(sx);
                    zs.push_back(sz);
                    int forward = 0;
                    bool forwardEnded = false;
                    while (forward < MAX_RUN)
                    {
                        const std::size_t at = selfOff + static_cast<std::size_t>(forward + 1) * stride;
                        if (at + tb > window.size())
                        {
                            forwardEnded = (hi == regionHi);
                            break;
                        }
                        float x = 0.0f, y = 0.0f, z = 0.0f;
                        if (!HuntDecodeTriple(window.data() + at, hit.fmt, x, y, z) || !HuntPlausibleHeight(y, sy))
                        {
                            forwardEnded = true;
                            break;
                        }
                        xs.push_back(x);
                        zs.push_back(z);
                        ++forward;
                    }
                    const int n = back + forward + 1;
                    if (!forwardEnded || n < 2 || n > MAX_RUN)
                        continue;

                    // Must contain at least one entity that is not us, and no duplicates.
                    bool other = false;
                    bool duplicate = false;
                    for (std::size_t a = 0; a < xs.size(); ++a)
                    {
                        if (!HuntIsSelf(xs[a], zs[a], sx, sz))
                            other = true;
                        for (std::size_t b = a + 1; b < xs.size(); ++b)
                        {
                            if (std::fabs(xs[a] - xs[b]) < 0.01f && std::fabs(zs[a] - zs[b]) < 0.01f)
                                duplicate = true;
                        }
                    }
                    if (!other || duplicate)
                        continue;

                    const uintptr_t start = hit.address - static_cast<uintptr_t>(back) * stride;
                    bool known = false;
                    for (const RunKey& key : seen)
                    {
                        if (key.fmt == hit.fmt && key.stride == stride && key.start == start)
                            known = true;
                    }
                    if (known)
                        break;
                    seen.push_back({ start, stride, hit.fmt });

                    HuntGroup group;
                    std::ostringstream label;
                    label << "run" << groups.size() << "@" << Hex(start) << "/" << Hex(stride) << "/" << HuntFormatName(hit.fmt);
                    group.label = label.str();
                    group.fmt = hit.fmt;
                    for (int k = 0; k < n; ++k)
                        group.addresses.push_back(start + static_cast<uintptr_t>(k) * stride);

                    std::ostringstream line;
                    line << "[Minimap] player hunt " << runIndex << " run | " << group.label << " | n=" << n << " self_index=" << back << " | xz=[";
                    for (std::size_t k = 0; k < xs.size() && k < 16; ++k)
                        line << (k ? " " : "") << static_cast<int>(xs[k]) << "," << static_cast<int>(zs[k]);
                    line << "]";
                    lines.push_back(line.str());
                    groups.push_back(std::move(group));
                    break;   // smallest stride only
                }
            }
        }

        // Pass 3: objects sharing an exe pointer at the same relative offset.
        if (HUNT_LEGACY_PASSES && g_exeBase != 0 && g_exeImageSize != 0)
        {
            const uintptr_t exeLo = g_exeBase;
            const uintptr_t exeHi = g_exeBase + g_exeImageSize;
            std::unordered_map<std::uint64_t, std::vector<HuntKeyUse>> keys;
            std::size_t keyCount = 0;
            std::size_t used = 0;
            for (const HuntHit& hit : hits)
            {
                if (used >= VPTR_HITS || keyCount >= MAX_KEYS)
                    break;
                ++used;
                std::uint8_t around[0x1C0] = {};
                const uintptr_t lo = (hit.address & ~static_cast<uintptr_t>(7)) - 0x180;
                uintptr_t regionLo = 0, regionHi = 0;
                // Stay inside the hit's own region: the page below a thread stack is a
                // guard page, and touching it would break that thread's stack growth.
                if (!HuntRegionBounds(hit.address, regionLo, regionHi) || lo < regionLo || lo + sizeof(around) > regionHi)
                    continue;
                if (!SafeRead(lo, around, sizeof(around)))
                    continue;
                for (std::size_t off = 0; off + 8 <= sizeof(around); off += 8)
                {
                    std::uint64_t value = 0;
                    std::memcpy(&value, around + off, 8);
                    if (value < exeLo || value >= exeHi)
                        continue;
                    const std::int32_t delta = static_cast<std::int32_t>(static_cast<std::int64_t>(lo + off) - static_cast<std::int64_t>(hit.address));
                    std::vector<HuntKeyUse>& uses = keys[value];
                    bool exists = false;
                    for (const HuntKeyUse& use : uses)
                    {
                        if (use.delta == delta && use.fmt == hit.fmt)
                            exists = true;
                    }
                    if (!exists)
                    {
                        HuntKeyUse use;
                        use.delta = delta;
                        use.fmt = hit.fmt;
                        uses.push_back(std::move(use));
                        ++keyCount;
                    }
                }
            }

            if (!keys.empty())
            {
                HuntForEachRegion([&](uintptr_t regionBase, std::size_t regionSize)
                {
                    for (std::size_t pos = 0; pos < regionSize; pos += CHUNK)
                    {
                        const std::size_t readBytes = MinValue<std::size_t>(CHUNK, regionSize - pos);
                        if (readBytes < 8 || !SafeRead(regionBase + pos, buffer.data(), readBytes))
                            continue;
                        for (std::size_t off = 0; off + 8 <= readBytes; off += 8)
                        {
                            std::uint64_t value = 0;
                            std::memcpy(&value, buffer.data() + off, 8);
                            if (value < exeLo || value >= exeHi)
                                continue;
                            const uintptr_t pointerAddress = regionBase + pos + off;
                            if (pointerAddress >= bufferLo && pointerAddress < bufferHi)
                                continue;
                            auto found = keys.find(value);
                            if (found == keys.end())
                                continue;
                            for (HuntKeyUse& use : found->second)
                            {
                                ++use.total;
                                if (use.total > 400)
                                    continue;
                                const uintptr_t positionAddress = pointerAddress - static_cast<uintptr_t>(static_cast<std::intptr_t>(use.delta));
                                if (positionAddress < regionBase || positionAddress + HuntTripleBytes(use.fmt) > regionBase + regionSize)
                                    continue;
                                float x = 0.0f, y = 0.0f, z = 0.0f;
                                if (!HuntReadTriple(positionAddress, use.fmt, x, y, z) || !HuntPlausibleHeight(y, sy))
                                    continue;
                                if (use.instances.size() < MAX_INSTANCES)
                                    use.instances.push_back(positionAddress);
                            }
                        }
                    }
                });

                struct Ranked { std::uint64_t vptr; const HuntKeyUse* use; };
                std::vector<Ranked> ranked;
                for (const auto& entry : keys)
                {
                    for (const HuntKeyUse& use : entry.second)
                    {
                        if (use.total > 64 || use.instances.size() < 2 || use.instances.size() > 24)
                            continue;
                        ranked.push_back({ entry.first, &use });
                    }
                }
                std::sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b)
                {
                    return a.use->instances.size() < b.use->instances.size();
                });

                std::vector<std::vector<uintptr_t>> seenSets;
                for (const Ranked& item : ranked)
                {
                    if (groups.size() >= MAX_GROUPS)
                        break;
                    std::vector<uintptr_t> set = item.use->instances;
                    std::sort(set.begin(), set.end());
                    bool known = false;
                    for (const auto& other : seenSets)
                    {
                        if (other == set)
                            known = true;
                    }
                    if (known)
                        continue;

                    std::vector<float> xs;
                    std::vector<float> zs;
                    bool other = false;
                    bool duplicate = false;
                    for (uintptr_t address : set)
                    {
                        float x = 0.0f, y = 0.0f, z = 0.0f;
                        HuntReadTriple(address, item.use->fmt, x, y, z);
                        for (std::size_t k = 0; k < xs.size(); ++k)
                        {
                            if (std::fabs(xs[k] - x) < 0.01f && std::fabs(zs[k] - z) < 0.01f)
                                duplicate = true;
                        }
                        xs.push_back(x);
                        zs.push_back(z);
                        if (!HuntIsSelf(x, z, sx, sz))
                            other = true;
                    }
                    if (!other || duplicate)
                        continue;
                    seenSets.push_back(set);

                    HuntGroup group;
                    std::ostringstream label;
                    label << "obj" << groups.size() << "@rva" << Hex(static_cast<uintptr_t>(item.vptr - g_exeBase))
                        << "/" << (item.use->delta < 0 ? "-" : "+") << Hex(static_cast<uintptr_t>(std::abs(item.use->delta)))
                        << "/" << HuntFormatName(item.use->fmt);
                    group.label = label.str();
                    group.fmt = item.use->fmt;
                    group.addresses = set;

                    std::ostringstream line;
                    line << "[Minimap] player hunt " << runIndex << " obj | " << group.label
                        << " | n=" << set.size() << " ptr_total=" << item.use->total << " | xz=[";
                    for (std::size_t k = 0; k < xs.size() && k < 16; ++k)
                        line << (k ? " " : "") << static_cast<int>(xs[k]) << "," << static_cast<int>(zs[k]);
                    line << "]";
                    lines.push_back(line.str());
                    groups.push_back(std::move(group));
                }
            }

            std::ostringstream oss;
            oss << "[Minimap] player hunt " << runIndex << " done | keys=" << keyCount
                << " | groups=" << groups.size() << " | ms=" << (GetTickCount() - startTick);
            Log(oss.str());
        }

        // Pass 4 ("twins"): other entities whose surrounding bytes share rare values with
        // the bytes around our own fixed-point position (same prefab/resource pointers,
        // type hashes). Values shared by only a handful of entities point at players.
        {
            constexpr std::ptrdiff_t TWIN_BEFORE = 0x200;
            constexpr std::ptrdiff_t TWIN_AFTER = 0x200;
            constexpr std::size_t TWIN_PAD = 0x200;
            constexpr std::uint32_t TWIN_RARE_MAX = 24;
            constexpr std::uint32_t TWIN_COMMON_CAP = 64;
            constexpr std::size_t TWIN_MAX_SELF = 400;
            constexpr std::size_t TWIN_MAX_MATCHES = 400000;
            constexpr std::size_t TWIN_TOP = 24;

            struct TwinFeature
            {
                std::int32_t delta = 0;
                std::uint64_t value = 0;
                std::uint32_t selfHits = 0;
                uintptr_t exampleSelf = 0;
                std::uint32_t count = 0;
                std::uint32_t lastCandidate = 0xFFFFFFFFu;
            };
            struct TwinCandidate
            {
                uintptr_t address = 0;
                float x = 0.0f;
                float y = 0.0f;
                float z = 0.0f;
            };

            const auto distinctive = [](std::uint64_t value)
            {
                if (value <= 0xFFFFu || value == ~0ULL)
                    return false;
                if ((value >> 32) == 0xFFFFFFFFu)
                    return false;   // small negative integer
                return true;
            };

            std::vector<TwinFeature> features;
            std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> byValue;
            std::size_t selfUsed = 0;
            std::vector<std::uint8_t> around(static_cast<std::size_t>(TWIN_BEFORE + TWIN_AFTER));
            for (const HuntHit& hit : hits)
            {
                if (hit.fmt != HUNT_I64 || selfUsed >= TWIN_MAX_SELF)
                    continue;
                uintptr_t regionLo = 0, regionHi = 0;
                if (!HuntRegionBounds(hit.address, regionLo, regionHi))
                    continue;
                const uintptr_t lo = hit.address - static_cast<uintptr_t>(TWIN_BEFORE);
                if (hit.address < regionLo + static_cast<uintptr_t>(TWIN_BEFORE) || hit.address + static_cast<uintptr_t>(TWIN_AFTER) > regionHi)
                    continue;
                if (!SafeRead(lo, around.data(), around.size()))
                    continue;
                ++selfUsed;
                for (std::ptrdiff_t d = -TWIN_BEFORE; d < TWIN_AFTER; d += 8)
                {
                    if (d >= 0 && d < 24)
                        continue;   // the position itself
                    std::uint64_t value = 0;
                    std::memcpy(&value, around.data() + static_cast<std::size_t>(d + TWIN_BEFORE), 8);
                    if (!distinctive(value))
                        continue;
                    const std::int32_t delta = static_cast<std::int32_t>(d);
                    std::vector<std::uint32_t>& ids = byValue[value];
                    bool merged = false;
                    for (std::uint32_t id : ids)
                    {
                        if (features[id].delta == delta)
                        {
                            ++features[id].selfHits;
                            merged = true;
                            break;
                        }
                    }
                    if (!merged)
                    {
                        TwinFeature feature;
                        feature.delta = delta;
                        feature.value = value;
                        feature.selfHits = 1;
                        feature.exampleSelf = hit.address;
                        ids.push_back(static_cast<std::uint32_t>(features.size()));
                        features.push_back(feature);
                    }
                }
            }

            std::vector<TwinCandidate> candidates;
            std::vector<std::pair<std::uint32_t, std::uint32_t>> matches;   // candidate, feature
            std::uint64_t positionsSeen = 0;
            if (!features.empty())
            {
                std::vector<std::uint8_t> padded(CHUNK + 2 * TWIN_PAD);
                const uintptr_t paddedLo = reinterpret_cast<uintptr_t>(padded.data());
                const uintptr_t paddedHi = paddedLo + padded.size();
                const std::int64_t mapHi = static_cast<std::int64_t>(REAL_MAP_WORLD_SIZE) + 64;
                HuntForEachRegion([&](uintptr_t regionBase, std::size_t regionSize)
                {
                    for (std::size_t pos = 0; pos < regionSize; pos += CHUNK)
                    {
                        const std::size_t before = MinValue<std::size_t>(TWIN_PAD, pos);
                        const std::size_t readStart = pos - before;
                        const std::size_t readBytes = MinValue<std::size_t>(before + CHUNK + TWIN_PAD, regionSize - readStart);
                        if (readBytes < 24 || !SafeRead(regionBase + readStart, padded.data(), readBytes))
                            continue;
                        const std::size_t coreEnd = MinValue<std::size_t>(before + CHUNK, readBytes);
                        for (std::size_t off = before; off + 24 <= coreEnd; off += 8)
                        {
                            std::int64_t v[3] = {};
                            std::memcpy(v, padded.data() + off, sizeof(v));
                            const std::int64_t hx = v[0] >> 32;
                            const std::int64_t hz = v[2] >> 32;
                            if (hx < 1 || hx > mapHi || hz < 1 || hz > mapHi)
                                continue;
                            float x = 0.0f, y = 0.0f, z = 0.0f;
                            if (!HuntDecodeTriple(padded.data() + off, HUNT_I64, x, y, z) || !HuntPlausibleHeight(y, sy))
                                continue;
                            if (HuntIsSelf(x, z, sx, sz))
                                continue;
                            const uintptr_t address = regionBase + readStart + off;
                            if (address >= paddedLo && address < paddedHi)
                                continue;
                            ++positionsSeen;

                            std::uint32_t candidateIndex = 0xFFFFFFFFu;
                            for (std::ptrdiff_t d = -TWIN_BEFORE; d < TWIN_AFTER; d += 8)
                            {
                                if (d >= 0 && d < 24)
                                    continue;
                                const std::ptrdiff_t at = static_cast<std::ptrdiff_t>(off) + d;
                                if (at < 0 || static_cast<std::size_t>(at) + 8 > readBytes)
                                    continue;
                                std::uint64_t value = 0;
                                std::memcpy(&value, padded.data() + static_cast<std::size_t>(at), 8);
                                if (!distinctive(value))
                                    continue;
                                auto found = byValue.find(value);
                                if (found == byValue.end())
                                    continue;
                                for (std::uint32_t id : found->second)
                                {
                                    TwinFeature& feature = features[id];
                                    if (feature.delta != static_cast<std::int32_t>(d))
                                        continue;
                                    if (candidateIndex == 0xFFFFFFFFu)
                                    {
                                        candidateIndex = static_cast<std::uint32_t>(candidates.size());
                                        candidates.push_back({ address, x, y, z });
                                    }
                                    if (feature.lastCandidate == candidateIndex)
                                        continue;
                                    feature.lastCandidate = candidateIndex;
                                    ++feature.count;
                                    if (feature.count <= TWIN_COMMON_CAP && matches.size() < TWIN_MAX_MATCHES)
                                        matches.push_back({ candidateIndex, id });
                                }
                            }
                        }
                    }
                });
            }

            // Score: number of rare features each candidate shares with us.
            std::vector<std::uint16_t> score(candidates.size(), 0);
            std::vector<std::uint32_t> bestFeature(candidates.size(), 0xFFFFFFFFu);
            std::size_t rareFeatures = 0;
            for (const TwinFeature& feature : features)
            {
                if (feature.count >= 1 && feature.count <= TWIN_RARE_MAX)
                    ++rareFeatures;
            }
            for (const auto& match : matches)
            {
                const TwinFeature& feature = features[match.second];
                if (feature.count < 1 || feature.count > TWIN_RARE_MAX)
                    continue;
                if (score[match.first] < 0xFFFF)
                    ++score[match.first];
                std::uint32_t& best = bestFeature[match.first];
                if (best == 0xFFFFFFFFu || features[best].count > feature.count)
                    best = match.second;
            }

            std::vector<std::uint32_t> order;
            for (std::uint32_t i = 0; i < candidates.size(); ++i)
            {
                if (score[i] >= 2)
                    order.push_back(i);
            }
            std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b)
            {
                return score[a] > score[b];
            });

            {
                std::ostringstream oss;
                oss << "[Minimap] player twin hunt " << runIndex
                    << " | self_structs=" << selfUsed
                    << " | features=" << features.size() << " rare=" << rareFeatures
                    << " | positions=" << positionsSeen << " matched=" << candidates.size()
                    << " scored=" << order.size()
                    << " | ms=" << (GetTickCount() - startTick);
                lines.push_back(oss.str());
            }

            // Most useful rare features (shared by the fewest other entities).
            {
                std::vector<std::uint32_t> rare;
                for (std::uint32_t id = 0; id < features.size(); ++id)
                {
                    if (features[id].count >= 1 && features[id].count <= 8)
                        rare.push_back(id);
                }
                std::sort(rare.begin(), rare.end(), [&](std::uint32_t a, std::uint32_t b)
                {
                    if (features[a].selfHits != features[b].selfHits)
                        return features[a].selfHits > features[b].selfHits;
                    return features[a].count < features[b].count;
                });
                std::ostringstream oss;
                oss << "[Minimap] player twin features " << runIndex;
                for (std::size_t k = 0; k < rare.size() && k < 16; ++k)
                {
                    const TwinFeature& f = features[rare[k]];
                    const bool exe = f.value >= g_exeBase && f.value < g_exeBase + g_exeImageSize;
                    oss << " | d=" << (f.delta < 0 ? "-" : "+") << Hex(static_cast<uintptr_t>(std::abs(f.delta)))
                        << " v=" << (exe ? "rva" : "") << Hex(static_cast<uintptr_t>(exe ? f.value - g_exeBase : f.value))
                        << " self=" << f.selfHits << " others=" << f.count;
                }
                lines.push_back(oss.str());
            }

            HuntGroup twins;
            twins.fmt = HUNT_I64;
            {
                std::ostringstream label;
                label << "twins" << runIndex;
                twins.label = label.str();
            }
            for (std::size_t k = 0; k < order.size() && k < TWIN_TOP; ++k)
            {
                const TwinCandidate& c = candidates[order[k]];
                const TwinFeature* f = bestFeature[order[k]] != 0xFFFFFFFFu ? &features[bestFeature[order[k]]] : nullptr;
                const float dx = c.x - sx;
                const float dz = c.z - sz;
                std::ostringstream line;
                line << "[Minimap] player twin " << runIndex << " #" << k
                    << " | score=" << score[order[k]]
                    << " | at=" << Hex(c.address)
                    << " | pos=(" << static_cast<int>(c.x) << "," << static_cast<int>(c.y) << "," << static_cast<int>(c.z) << ")"
                    << " d=" << static_cast<int>(std::sqrt(dx * dx + dz * dz));
                if (f != nullptr)
                {
                    line << " | via self=" << Hex(f->exampleSelf)
                        << " d=" << (f->delta < 0 ? "-" : "+") << Hex(static_cast<uintptr_t>(std::abs(f->delta)))
                        << " v=" << Hex(static_cast<uintptr_t>(f->value)) << " others=" << f->count;
                }
                lines.push_back(line.str());
                twins.addresses.push_back(c.address);
            }
            if (!twins.addresses.empty())
                groups.push_back(std::move(twins));
        }

        for (const std::string& line : lines)
            Log(line);

        std::lock_guard<std::mutex> lock(g_huntMutex);
        for (HuntGroup& group : groups)
        {
            if (g_huntGroups.size() >= 120)
                break;
            group.moves.assign(group.addresses.size(), 0);
            g_huntGroups.push_back(std::move(group));
        }
        g_huntWatchUntilTick = GetTickCount() + 240000;
    }

    // ---- PING HUNT (debug only) ----
    // Player pings (MapMarkerRegistry "mapmarker_playerPing") never show up in the master
    // marker array. Every 10 s this scans the heap for marker-type keys stored next to a
    // world position and logs the ones that are new, so a ping placed in game can be
    // located in memory.
    struct PingHuntKey
    {
        std::uint32_t key;
        const char* name;
    };

    constexpr PingHuntKey PING_HUNT_KEYS[] = {
        { 0x83405288u, "playerPing" },
        { 0xEB250F27u, "attention" },
        { 0x7103269Eu, "custom_glider" },
        { 0x28A5FB2Cu, "custom_wildlife" },
        { 0xA1C4ECE9u, "custom_loot" },
        { 0x5EBA3084u, "custom_goal" },
        { 0xD9B19AECu, "custom_plants" },
        { 0xEEB09ABEu, "custom_ore" },
        { 0x0E6558FFu, "custom_combat" },
        { 0x3A761350u, "custom_chest" },
        { 0x1DB9432Du, "custom_water" },
        { 0xFE2FD02Du, "custom_fish" },
    };

    struct PingHuntHit
    {
        uintptr_t address = 0;
        std::uint32_t key = 0;
        std::int32_t rel = 0;
        int fmt = HUNT_I64;
        float x = 0.0f;
        float z = 0.0f;
    };

    std::atomic<bool> g_pingHuntBusy{ false };
    std::atomic<DWORD> g_pingHuntLastTick{ 0 };
    std::mutex g_pingHuntMutex;
    std::vector<PingHuntHit> g_pingHuntKnown;
    int g_pingHuntRuns = 0;

    void RunPingHunt()
    {
        constexpr std::size_t CHUNK = 0x10000;
        constexpr std::size_t PAD = 0x40;
        constexpr std::size_t MAX_HITS = 2000;
        constexpr float PING_HUNT_RADIUS = 1500.0f;

        float sx = 0.0f, sy = 0.0f, sz = 0.0f;
        if (!HuntGetSelf(sx, sy, sz))
            return;

        std::array<bool, 256> lowByte{};
        for (const PingHuntKey& k : PING_HUNT_KEYS)
            lowByte[k.key & 0xFF] = true;

        const DWORD startTick = GetTickCount();
        std::vector<std::uint8_t> buffer(CHUNK + 2 * PAD);
        const uintptr_t bufferLo = reinterpret_cast<uintptr_t>(buffer.data());
        const uintptr_t bufferHi = bufferLo + buffer.size();
        std::vector<PingHuntHit> hits;

        HuntForEachRegion([&](uintptr_t regionBase, std::size_t regionSize)
        {
            for (std::size_t pos = 0; pos < regionSize && hits.size() < MAX_HITS; pos += CHUNK)
            {
                // Read the chunk plus padding on both sides, clamped to the region.
                const std::size_t before = MinValue<std::size_t>(PAD, pos);
                const std::size_t readStart = pos - before;
                const std::size_t readBytes = MinValue<std::size_t>(before + CHUNK + PAD, regionSize - readStart);
                if (readBytes < 16 || !SafeRead(regionBase + readStart, buffer.data(), readBytes))
                    continue;
                const std::size_t scanEnd = MinValue<std::size_t>(before + CHUNK, readBytes);
                for (std::size_t off = before; off + 4 <= scanEnd; off += 4)
                {
                    const std::uint8_t* p = buffer.data() + off;
                    if (!lowByte[p[0]])
                        continue;
                    std::uint32_t value = 0;
                    std::memcpy(&value, p, 4);
                    bool match = false;
                    for (const PingHuntKey& k : PING_HUNT_KEYS)
                        match = match || k.key == value;
                    if (!match)
                        continue;
                    const uintptr_t address = regionBase + readStart + off;
                    if (address >= bufferLo && address < bufferHi)
                        continue;

                    // Nearest on-map position within +-0x40 bytes.
                    bool found = false;
                    PingHuntHit hit{};
                    for (std::size_t dist = 4; dist <= PAD && !found; dist += 4)
                    {
                        for (int sign = -1; sign <= 1 && !found; sign += 2)
                        {
                            if (sign < 0 && dist > off)
                                continue;
                            const std::size_t at = sign < 0 ? off - dist : off + dist;
                            for (int fmt = 0; fmt < 2 && !found; ++fmt)
                            {
                                const int f = fmt == 0 ? HUNT_I64 : HUNT_F32;
                                if (at + HuntTripleBytes(f) > readBytes)
                                    continue;
                                if (f == HUNT_I64 && ((regionBase + readStart + at) & 7) != 0)
                                    continue;
                                float x = 0.0f, y = 0.0f, z = 0.0f;
                                if (!HuntDecodeTriple(buffer.data() + at, f, x, y, z))
                                    continue;
                                // A ping is placed within sight: ignore far or odd-height
                                // triples (the first run matched thousands of garbage ones).
                                // Map markers may carry y = 0, so accept that too.
                                const float ddx = x - sx;
                                const float ddz = z - sz;
                                if (ddx * ddx + ddz * ddz > PING_HUNT_RADIUS * PING_HUNT_RADIUS)
                                    continue;
                                if (!(std::fabs(y) < 0.01f || HuntPlausibleHeight(y, sy)))
                                    continue;
                                found = true;
                                hit.address = address;
                                hit.key = value;
                                hit.rel = sign * static_cast<std::int32_t>(dist);
                                hit.fmt = f;
                                hit.x = x;
                                hit.z = z;
                            }
                        }
                    }
                    if (found)
                        hits.push_back(hit);
                }
            }
        });

        std::lock_guard<std::mutex> lock(g_pingHuntMutex);
        const bool first = g_pingHuntRuns == 0;
        ++g_pingHuntRuns;

        std::array<int, sizeof(PING_HUNT_KEYS) / sizeof(PING_HUNT_KEYS[0])> perKey{};
        int newCount = 0;
        int gone = 0;
        std::ostringstream detail;
        for (const PingHuntHit& hit : hits)
        {
            for (std::size_t k = 0; k < perKey.size(); ++k)
            {
                if (PING_HUNT_KEYS[k].key == hit.key)
                    ++perKey[k];
            }
            bool known = false;
            for (const PingHuntHit& old : g_pingHuntKnown)
            {
                if (old.address == hit.address && old.key == hit.key &&
                    std::fabs(old.x - hit.x) < 0.5f && std::fabs(old.z - hit.z) < 0.5f)
                {
                    known = true;
                    break;
                }
            }
            if (known || first)
                continue;
            if (++newCount > 24)
                continue;
            const char* name = "?";
            for (const PingHuntKey& k : PING_HUNT_KEYS)
            {
                if (k.key == hit.key)
                    name = k.name;
            }
            const float dx = hit.x - sx;
            const float dz = hit.z - sz;
            detail << " | " << name << "@" << Hex(hit.address)
                << " pos" << (hit.rel < 0 ? "-" : "+") << Hex(static_cast<uintptr_t>(std::abs(hit.rel)))
                << "/" << HuntFormatName(hit.fmt)
                << " (" << static_cast<int>(hit.x) << "," << static_cast<int>(hit.z) << ")"
                << " d=" << static_cast<int>(std::sqrt(dx * dx + dz * dz));
        }
        for (const PingHuntHit& old : g_pingHuntKnown)
        {
            bool still = false;
            for (const PingHuntHit& hit : hits)
            {
                if (old.address == hit.address && old.key == hit.key)
                {
                    still = true;
                    break;
                }
            }
            if (!still)
            {
                if (++gone <= 12)
                {
                    const char* name = "?";
                    for (const PingHuntKey& k : PING_HUNT_KEYS)
                    {
                        if (k.key == old.key)
                            name = k.name;
                    }
                    detail << " | gone " << name << "@" << Hex(old.address)
                        << " (" << static_cast<int>(old.x) << "," << static_cast<int>(old.z) << ")";
                }
            }
        }

        if (first || newCount != 0 || gone != 0)
        {
            std::ostringstream oss;
            oss << "[Minimap] ping hunt " << g_pingHuntRuns
                << " | self=(" << static_cast<int>(sx) << "," << static_cast<int>(sz) << ")"
                << " | hits=" << hits.size() << " new=" << newCount << " gone=" << gone
                << " | ms=" << (GetTickCount() - startTick) << " |";
            for (std::size_t k = 0; k < perKey.size(); ++k)
            {
                if (perKey[k] != 0)
                    oss << " " << PING_HUNT_KEYS[k].name << "=" << perKey[k];
            }
            oss << detail.str();
            Log(oss.str());
        }
        g_pingHuntKnown = std::move(hits);
    }

    void MaybeStartPingHunt()
    {
        const DWORD now = GetTickCount();
        const DWORD last = g_pingHuntLastTick.load();
        if (last != 0 && now - last < 10000)
            return;
        if (g_huntBusy.load() || g_pingHuntBusy.exchange(true))
            return;
        g_pingHuntLastTick.store(now);
        std::thread([]()
        {
            BackgroundThreadScope scope;
            RunPingHunt();
            g_pingHuntBusy.store(false);
        }).detach();
    }
    // ---- END PING HUNT ----

    // The memory hunts found the player list and the ping queue (9/18); they stay in the
    // source for future game updates but are off, since each scan costs seconds of CPU.
    constexpr bool MEMORY_HUNTS_ENABLED = false;
    // The per-marker hook inside knowledge_query_mapmarker_visibility ran once per
    // marker per frame on the game thread (SEH copy, a memory read and a mutex each)
    // and, on the September 2026 client, produced a single bogus entry: the master
    // marker arrays carry everything the world map shows. Off by default.
    constexpr bool MARKER_VISIBILITY_HOOK_ENABLED = false;

    void RunRemotePlayerProbe(std::uint8_t* state)
    {
        (void)state;
        if (!MEMORY_HUNTS_ENABLED || !g_debugLoggingEnabled.load() || !MinimapRuntimeActive())
            return;

        const DWORD now = GetTickCount();
        float sx = 0.0f, sy = 0.0f, sz = 0.0f;
        if (!HuntGetSelf(sx, sy, sz))
            return;
        if (g_huntReadyTick == 0)
            g_huntReadyTick = now;
        if (now - g_huntReadyTick >= 15000)
            MaybeStartPingHunt();

        // Hunt 20 s after the world is ready, and again after 2 and 4 minutes
        // (other players may have joined or moved by then).
        const DWORD sinceReady = now - g_huntReadyTick;
        const DWORD dueAt = g_huntRuns == 0 ? 20000 : (g_huntRuns == 1 ? 120000 : 240000);
        if (g_huntRuns < 3 && sinceReady >= dueAt && !g_huntBusy.exchange(true))
        {
            const int runIndex = ++g_huntRuns;
            std::thread([runIndex]()
            {
                BackgroundThreadScope scope;
                RunRemotePlayerHunt(runIndex);
                g_huntBusy.store(false);
            }).detach();
        }

        std::lock_guard<std::mutex> lock(g_huntMutex);
        if (g_huntGroups.empty() || now > g_huntWatchUntilTick)
            return;

        if (now - g_huntLastSampleTick >= 1000)
        {
            g_huntLastSampleTick = now;
            for (HuntGroup& group : g_huntGroups)
            {
                std::vector<float> xz;
                bool self = false;
                for (std::size_t k = 0; k < group.addresses.size(); ++k)
                {
                    float x = 0.0f, y = 0.0f, z = 0.0f;
                    if (!HuntReadTriple(group.addresses[k], group.fmt, x, y, z))
                    {
                        x = 0.0f;
                        z = 0.0f;
                    }
                    xz.push_back(x);
                    xz.push_back(z);
                    const bool isSelf = HuntIsSelf(x, z, sx, sz);
                    self = self || isSelf;
                    if (!isSelf && group.lastXZ.size() == group.addresses.size() * 2 &&
                        (std::fabs(group.lastXZ[k * 2] - x) > 0.3f || std::fabs(group.lastXZ[k * 2 + 1] - z) > 0.3f))
                    {
                        ++group.moves[k];
                    }
                }
                ++group.samples;
                if (self)
                    ++group.selfHits;
                group.lastXZ = std::move(xz);
            }
        }

        if (now - g_huntLastLogTick >= 20000)
        {
            g_huntLastLogTick = now;
            int logged = 0;
            for (const HuntGroup& group : g_huntGroups)
            {
                std::uint32_t movingEntries = 0;
                for (std::uint32_t m : group.moves)
                {
                    if (m >= 3)
                        ++movingEntries;
                }
                if (movingEntries == 0)
                    continue;
                std::ostringstream oss;
                oss << "[Minimap] player hunt watch | " << group.label
                    << " | n=" << group.addresses.size()
                    << " | moving=" << movingEntries
                    << " | self=" << group.selfHits << "/" << group.samples
                    << " | moves=[";
                for (std::size_t k = 0; k < group.moves.size() && k < 16; ++k)
                    oss << (k ? " " : "") << group.moves[k];
                oss << "] | xz=[";
                for (std::size_t k = 0; k + 1 < group.lastXZ.size() && k < 32; k += 2)
                    oss << (k ? " " : "") << static_cast<int>(group.lastXZ[k]) << "," << static_cast<int>(group.lastXZ[k + 1]);
                oss << "]";
                Log(oss.str());
                if (++logged >= 30)
                    break;
            }
            if (logged == 0)
            {
                std::ostringstream oss;
                oss << "[Minimap] player hunt watch | groups=" << g_huntGroups.size() << " | none moving yet";
                Log(oss.str());
            }
        }
    }
    // ---- END REMOTE PLAYER PROBE ----

    bool TryCaptureWaypointsFromPlayerWaypointsUi(void* ctx)
    {
        if (g_iterInit == nullptr || ctx == nullptr)
            return false;

        WaypointsUiIterationRecord record{};
        if (!TryInitWaypointRecord(ctx, record))
            return false;


        std::uint8_t* state = ResolveWaypointsUiState(record);
        if (state == nullptr)
            return false;
        if (!AcceptUiState(state))
            return false;

        // Keep the position feed alive from this per-frame hook as well.
        TryPublishExactUiStatePosition(state);

        // Everything below mirrors slow-moving world data (the master marker array is a
        // ~400 KB copy plus track matching; the player list and the streamed marker lists
        // are more of the same). This hook runs on the game's main thread every frame, so
        // it is capped at 20 Hz - well above what a marker or a teammate dot needs, and it
        // takes the cost off the frame that is already busy. The player position and the
        // camera heading above stay at full frame rate.
        {
            static DWORD lastSlowCaptureTick = 0;
            const DWORD nowTick = GetTickCount();
            if (lastSlowCaptureTick == 0 || nowTick - lastSlowCaptureTick >= 50)
            {
                lastSlowCaptureTick = nowTick;

                TryCaptureMasterMarkers(state);
                TryCaptureRemotePlayers(state);
                LogRemotePlayersIfDue();
                RunRemotePlayerProbe(state);

                std::vector<CapturedNearbyMarker> nearbyMarkers;
                AppendNearbyMarkersFromState(state, nearbyMarkers);
                AppendNearbyMarkersFromInputList(record.waypointList, nearbyMarkers, 2);
                AppendNearbyMarkersFromInputList(record.playerList, nearbyMarkers, 3);
                PublishNearbyMarkers(std::move(nearbyMarkers));
            }
        }

        WaypointBlockLayout layout{};
        std::uint8_t* entries = nullptr;
        std::uint64_t count = 0;
        if (!TryResolveWaypointBlock(state, layout, entries, count))
            return false;

        if (entries == nullptr || count == 0 || count > WAYPOINT_MAX_ENTRIES)
        {
            PublishWaypoints({});
            return true;
        }

        std::vector<CapturedWaypoint> waypoints;
        waypoints.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(count, 64)));
        const std::size_t activeFlagOffsets[] = { WAYPOINT_ENTRY_ACTIVE_OFFSET, 0x40, 0x50, 0x58, 0x60 };

        for (std::uint64_t index = 0; index < count; ++index)
        {
            std::uint8_t* entry = entries + (index * layout.stride);

            CapturedWaypoint waypoint{};
            SafeRead(reinterpret_cast<uintptr_t>(entry), waypoint.raw, sizeof(waypoint.raw));
            if (!SafeReadValue(reinterpret_cast<uintptr_t>(entry + layout.positionOffset + 0x00), waypoint.x) ||
                !SafeReadValue(reinterpret_cast<uintptr_t>(entry + layout.positionOffset + 0x08), waypoint.y) ||
                !SafeReadValue(reinterpret_cast<uintptr_t>(entry + layout.positionOffset + 0x10), waypoint.z))
            {
                continue;
            }

            SafeReadValue(reinterpret_cast<uintptr_t>(entry), waypoint.id);
            if (waypoint.id == 0)
            {
                // May-24-2026 layout keeps the marker id/type words at entry+0x30.
                std::uint32_t altId = 0;
                if (SafeReadValue(reinterpret_cast<uintptr_t>(entry + 0x30), altId))
                    waypoint.id = altId;
            }
            if (!IsPlausibleWorldPosition(FixedToWorld(waypoint.x), FixedToWorld(waypoint.y), FixedToWorld(waypoint.z)))
                continue;

            bool active = waypoint.id != 0;
            for (std::size_t activeOffset : activeFlagOffsets)
            {
                std::uint8_t activeByte = 0;
                if (SafeReadValue(reinterpret_cast<uintptr_t>(entry + activeOffset), activeByte) && activeByte != 0)
                {
                    active = true;
                    break;
                }
            }

            if (!active)
                continue;

            PushWaypointUnique(waypoints, waypoint);
        }

        PublishWaypoints(std::move(waypoints));
        return true;
    }

    // ---- fog of war capture (game thread, fog_of_war system entry) ----
    struct FogOfWarHeader
    {
        float sizeX = 0.0f;
        float sizeZ = 0.0f;
        std::uint32_t widthBlocks = 0;
        std::uint32_t heightBlocks = 0;
        std::uint64_t allocator = 0;
        std::uint64_t blocks = 0;
        std::uint64_t blockCount = 0;
    };
    static_assert(sizeof(FogOfWarHeader) == 0x28, "Unexpected FogOfWarHeader size");

    bool FogOfWarHeaderPlausible(const FogOfWarHeader& header)
    {
        return std::isfinite(header.sizeX) && std::isfinite(header.sizeZ) &&
            header.sizeX >= 512.0f && header.sizeX <= 65536.0f &&
            header.sizeZ >= 512.0f && header.sizeZ <= 65536.0f &&
            header.widthBlocks >= 1 && header.widthBlocks <= 256 &&
            header.heightBlocks >= 1 && header.heightBlocks <= 256 &&
            header.blockCount == static_cast<std::uint64_t>(header.widthBlocks) * header.heightBlocks &&
            IsLikelyRuntimePointer(static_cast<uintptr_t>(header.blocks));
    }

    // Walks the system's query (restoring its cursor afterwards, see
    // TryInitUiRenderSetupRecord) and returns the first plausible FogOfWar object.
    // No C++ objects with destructors inside: SEH frame.
    std::uint8_t* FindFogOfWarObjectGuarded(void* ctx, FogOfWarHeader* outHeader)
    {
        __try
        {
            auto* cursor = reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(ctx) + 0x08);
            const std::uint32_t savedCursor = *cursor;
            std::uint8_t* found = nullptr;
            alignas(16) std::uint8_t scratch[ITER_RECORD_SCRATCH_BYTES] = {};
            auto* record = reinterpret_cast<FogOfWarIterationRecord*>(scratch);
            g_iterInit(ctx, scratch, static_cast<std::uint32_t>(sizeof(FogOfWarIterationRecord)));
            for (int guard = 0; guard < 8 && found == nullptr; ++guard)
            {
                if (!g_iterNext(ctx, scratch, static_cast<std::uint32_t>(sizeof(FogOfWarIterationRecord))))
                    break;
                if (record->fogOfWar == nullptr || !IsLikelyRuntimePointer(reinterpret_cast<uintptr_t>(record->fogOfWar)))
                    continue;
                FogOfWarHeader header{};
                if (SafeRead(reinterpret_cast<uintptr_t>(record->fogOfWar), &header, sizeof(header)) && FogOfWarHeaderPlausible(header))
                {
                    *outHeader = header;
                    found = record->fogOfWar;
                }
            }
            *cursor = savedCursor;
            return found;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    std::mutex g_fogCaptureMutex;

    void TryCaptureFogOfWar(void* ctx)
    {
        // The ECS may run this system on any worker thread; never two captures at once.
        std::unique_lock<std::mutex> captureLock(g_fogCaptureMutex, std::try_to_lock);
        if (!captureLock.owns_lock())
            return;

        FogOfWarHeader header{};
        std::uint8_t* object = FindFogOfWarObjectGuarded(ctx, &header);
        if (object == nullptr)
            return;

        const std::size_t blockBytes = static_cast<std::size_t>(header.blockCount) * 1024;
        static std::vector<std::uint8_t> raw;      // guarded by g_fogCaptureMutex
        static std::vector<std::uint8_t> cells;
        raw.resize(blockBytes);
        if (!SafeRead(static_cast<uintptr_t>(header.blocks), raw.data(), blockBytes))
            return;

        // De-block: cell (x, y) lives in block (x >> 5, y >> 5) at (y & 31) * 32 + (x & 31).
        const std::uint32_t width = header.widthBlocks * 32;
        const std::uint32_t height = header.heightBlocks * 32;
        cells.resize(static_cast<std::size_t>(width) * height);
        for (std::uint32_t by = 0; by < header.heightBlocks; ++by)
        {
            for (std::uint32_t bx = 0; bx < header.widthBlocks; ++bx)
            {
                const std::uint8_t* block = raw.data() + (static_cast<std::size_t>(by) * header.widthBlocks + bx) * 1024;
                for (std::uint32_t row = 0; row < 32; ++row)
                {
                    std::memcpy(
                        cells.data() + (static_cast<std::size_t>(by) * 32 + row) * width + static_cast<std::size_t>(bx) * 32,
                        block + row * 32,
                        32);
                }
            }
        }

        std::size_t discovered = 0;
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(g_fogGridMutex);
            FogOfWarGrid& grid = g_fogGrid;
            changed = !grid.valid || grid.width != width || grid.height != height || grid.cells != cells;
            if (changed)
            {
                grid.cells = cells;
                grid.width = width;
                grid.height = height;
                grid.sizeX = header.sizeX;
                grid.sizeZ = header.sizeZ;
                grid.valid = true;
                ++grid.version;
            }
            grid.address = reinterpret_cast<uintptr_t>(object);
            grid.lastCaptureTick = GetTickCount();
        }

        if (!g_fogGridLogged.exchange(true) || (changed && g_debugLoggingEnabled.load()))
        {
            for (std::uint8_t value : cells)
                discovered += value >= 64 ? 1 : 0;
            std::ostringstream oss;
            oss << "[Minimap] fog of war grid"
                << " | object=" << Hex(reinterpret_cast<uintptr_t>(object))
                << " | world=" << header.sizeX << "x" << header.sizeZ
                << " | blocks=" << header.widthBlocks << "x" << header.heightBlocks
                << " | cells=" << width << "x" << height
                << " | discovered=" << (discovered * 100 / MaxValue<std::size_t>(1, cells.size())) << "%";
            Log(oss.str());
        }
    }

    void __fastcall CaptureFogOfWarHook(void* ctx, void*, void*, void*)
    {
        if (!g_minimapEnabled.load() || g_iterInit == nullptr || g_iterNext == nullptr || ctx == nullptr)
            return;
        // The grid only grows as you walk; 2 Hz keeps the copy (1.6 MB) negligible.
        static DWORD lastCaptureTick = 0;
        const DWORD now = GetTickCount();
        if (lastCaptureTick != 0 && now - lastCaptureTick < 500)
            return;
        lastCaptureTick = now;
        TryCaptureFogOfWar(ctx);
    }

    void __fastcall CaptureUiRenderSetupHook(void* ctx, void*, void*, void*)
    {
        if (!g_minimapEnabled.load())
            return;
        TryCaptureUiRenderSetup(ctx);
    }

    void __fastcall CaptureWaypointsHook(void* ctx, void*, void*, void*)
    {
        if (!g_minimapEnabled.load())
            return;
        if (!TryCaptureWaypointsFromPlayerWaypointsUi(ctx))
        {
            const DWORD now = GetTickCount();
            const DWORD last = g_lastWaypointFailureLogTick.load();
            if (last == 0 || now - last >= 10000)
            {
                g_lastWaypointFailureLogTick.store(now);
                Log("[Minimap] failed to read internal player_waypoints_ui state (repeats suppressed for 10s)");
            }
        }
    }

    void __fastcall CaptureRenderPresentFrameHook(void* renderContext, void*, void* swapchainState, void*)
    {
        const DWORD now = GetTickCount();
        if (now - g_lastRenderFrameSummaryTick < 5000)
            return;

        g_lastRenderFrameSummaryTick = now;

        uintptr_t table = 0;
        uintptr_t device = 0;
        uintptr_t graphicsContext = 0;
        uintptr_t queueState = 0;
        std::uint32_t queueIndex = 0;
        std::uint64_t presentCount = 0;
        uintptr_t presentSwapchains = 0;

        if (renderContext != nullptr)
        {
            SafeReadValue(reinterpret_cast<uintptr_t>(renderContext) + 0x08, table);
            SafeReadValue(reinterpret_cast<uintptr_t>(renderContext) + 0x10, device);
            SafeReadValue(reinterpret_cast<uintptr_t>(renderContext) + 0x28, graphicsContext);
        }

        g_lastRenderContext.store(reinterpret_cast<uintptr_t>(renderContext));
        g_lastVulkanDevice.store(device);
        g_lastGraphicsContext.store(graphicsContext);
        g_lastSwapchainState.store(reinterpret_cast<uintptr_t>(swapchainState));

        if (table != 0)
            TryInstallVulkanTableHooks(table);

        if (graphicsContext != 0)
        {
            SafeReadValue(graphicsContext + 0xD98, queueState);
            SafeReadValue(graphicsContext + 0xDFC, queueIndex);
            g_lastVulkanQueueFamilyIndex.store(queueIndex);
        }

        if (swapchainState != nullptr)
        {
            SafeReadValue(reinterpret_cast<uintptr_t>(swapchainState) + 0x120, presentCount);
            SafeReadValue(reinterpret_cast<uintptr_t>(swapchainState) + 0x118, presentSwapchains);
        }

        if (!g_debugLoggingEnabled.load())
            return;

        std::ostringstream oss;
        oss << "[Minimap] render present frame"
            << " | render_context=" << Hex(reinterpret_cast<uintptr_t>(renderContext))
            << " | table=" << Hex(table)
            << " | device=" << Hex(device)
            << " | graphics_context=" << Hex(graphicsContext)
            << " | queue_state=" << Hex(queueState)
            << " | queue_index=" << queueIndex
            << " | swapchain_state=" << Hex(reinterpret_cast<uintptr_t>(swapchainState))
            << " | present_count=" << presentCount
            << " | present_swapchains=" << Hex(presentSwapchains);
        Log(oss.str());
    }

    bool ReadVulkanTableFunction(uintptr_t table, std::size_t offset, uintptr_t& outValue)
    {
        outValue = 0;
        if (table == 0)
            return false;
        return SafeReadValue(table + offset, outValue);
    }

    bool IsReadablePage(DWORD protect)
    {
        if ((protect & PAGE_GUARD) != 0 || (protect & PAGE_NOACCESS) != 0)
            return false;

        switch (protect & 0xFF)
        {
        case PAGE_READONLY:
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
        }
    }

    bool WritePointer(uintptr_t address, uintptr_t value)
    {
        DWORD oldProtect = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(address), sizeof(value), PAGE_READWRITE, &oldProtect))
            return false;

        bool wrote = false;
        __try
        {
            *reinterpret_cast<uintptr_t*>(address) = value;
            wrote = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            wrote = false;
        }

        DWORD ignored = 0;
        VirtualProtect(reinterpret_cast<void*>(address), sizeof(value), oldProtect, &ignored);
        return wrote;
    }

    SwapchainRuntimeInfo* FindSwapchainLocked(uintptr_t handle)
    {
        auto found = std::find_if(
            g_swapchains.begin(),
            g_swapchains.end(),
            [handle](const SwapchainRuntimeInfo& info)
            {
                return info.handle == handle;
            });

        return found != g_swapchains.end() ? &(*found) : nullptr;
    }

    bool TryCopySwapchainCreateInfo(const VkSwapchainCreateInfoKHR* createInfo, VkSwapchainCreateInfoKHR& outInfo)
    {
        if (createInfo == nullptr)
            return false;

        return SafeReadValue(reinterpret_cast<uintptr_t>(createInfo), outInfo);
    }

    void RememberSwapchainCreate(void* device, const VkSwapchainCreateInfoKHR* createInfo, void* swapchain, std::int32_t result)
    {
        if (result != 0 || createInfo == nullptr || swapchain == nullptr)
            return;

        VkSwapchainCreateInfoKHR info{};
        if (!TryCopySwapchainCreateInfo(createInfo, info))
            return;

        std::lock_guard<std::mutex> lock(g_swapchainMutex);
        if (info.oldSwapchain != nullptr)
        {
            const uintptr_t oldHandle = reinterpret_cast<uintptr_t>(info.oldSwapchain);
            g_swapchains.erase(
                std::remove_if(
                    g_swapchains.begin(),
                    g_swapchains.end(),
                    [oldHandle](const SwapchainRuntimeInfo& existing)
                    {
                        return existing.handle == oldHandle;
                    }),
                g_swapchains.end());
        }

        const uintptr_t handle = reinterpret_cast<uintptr_t>(swapchain);
        SwapchainRuntimeInfo* runtime = FindSwapchainLocked(handle);
        if (runtime == nullptr)
        {
            g_swapchains.push_back({});
            runtime = &g_swapchains.back();
        }

        runtime->handle = handle;
        runtime->device = reinterpret_cast<uintptr_t>(device);
        runtime->format = info.imageFormat;
        runtime->width = info.imageExtent.width;
        runtime->height = info.imageExtent.height;
        runtime->minImageCount = info.minImageCount;
        runtime->imageCount = 0;
        runtime->images.clear();
    }

    void RememberSwapchainImages(void* device, void* swapchain, std::uint32_t* count, void* images, std::int32_t result)
    {
        if (result != 0 || swapchain == nullptr || count == nullptr)
            return;

        std::uint32_t imageCount = 0;
        if (!SafeReadValue(reinterpret_cast<uintptr_t>(count), imageCount) || imageCount > 16)
            return;

        std::vector<uintptr_t> imageHandles;
        if (images != nullptr && imageCount != 0)
        {
            imageHandles.resize(imageCount);
            if (!SafeRead(reinterpret_cast<uintptr_t>(images), imageHandles.data(), sizeof(uintptr_t) * imageHandles.size()))
                imageHandles.clear();
        }

        std::lock_guard<std::mutex> lock(g_swapchainMutex);
        const uintptr_t handle = reinterpret_cast<uintptr_t>(swapchain);
        SwapchainRuntimeInfo* runtime = FindSwapchainLocked(handle);
        if (runtime == nullptr)
        {
            g_swapchains.push_back({});
            runtime = &g_swapchains.back();
            runtime->handle = handle;
        }

        if (device != nullptr)
            runtime->device = reinterpret_cast<uintptr_t>(device);

        runtime->imageCount = imageCount;
        if (!imageHandles.empty())
            runtime->images = std::move(imageHandles);
    }

    bool TryGetSwapchainSnapshot(uintptr_t handle, SwapchainRuntimeInfo& outInfo)
    {
        std::lock_guard<std::mutex> lock(g_swapchainMutex);
        SwapchainRuntimeInfo* runtime = FindSwapchainLocked(handle);
        if (runtime == nullptr)
            return false;

        outInfo = *runtime;
        return true;
    }

    std::size_t FindVulkanTableOffset(uintptr_t table, uintptr_t function)
    {
        if (table == 0 || function == 0)
            return static_cast<std::size_t>(-1);

        for (std::size_t offset = 0; offset <= VULKAN_DEVICE_TABLE_SCAN_LIMIT; offset += sizeof(uintptr_t))
        {
            uintptr_t current = 0;
            if (SafeReadValue(table + offset, current) && current == function)
                return offset;
        }

        return static_cast<std::size_t>(-1);
    }

    void AppendFunctionLookup(std::ostringstream& oss, uintptr_t table, GetDeviceProcAddrFn getDeviceProcAddr, void* device, const char* name)
    {
        const uintptr_t pointer = reinterpret_cast<uintptr_t>(getDeviceProcAddr(device, name));
        const std::size_t offset = FindVulkanTableOffset(table, pointer);

        oss << " | " << name << "=" << Hex(pointer);
        if (offset != static_cast<std::size_t>(-1))
            oss << "@" << Hex(static_cast<uintptr_t>(offset));
        else if (pointer != 0)
            oss << "@external";
        else
            oss << "@missing";
    }

    void TryScanVulkanDeviceFunctions()
    {
        const DWORD now = GetTickCount();
        if (g_vulkanFunctionOffsetsLogged || now - g_lastVulkanFunctionScanTick < 10000)
            return;

        std::lock_guard<std::mutex> lock(g_vulkanFunctionScanMutex);
        if (g_vulkanFunctionOffsetsLogged || now - g_lastVulkanFunctionScanTick < 10000)
            return;

        g_lastVulkanFunctionScanTick = now;

        const uintptr_t table = g_vulkanDeviceTable.load();
        const uintptr_t device = g_lastVulkanDevice.load();
        if (table == 0 || device == 0)
            return;

        uintptr_t getDeviceProcAddrValue = 0;
        if (!ReadVulkanTableFunction(table, VULKAN_TABLE_GET_DEVICE_PROC_ADDR_OFFSET, getDeviceProcAddrValue) ||
            getDeviceProcAddrValue == 0)
        {
            return;
        }

        auto* getDeviceProcAddr = reinterpret_cast<GetDeviceProcAddrFn>(getDeviceProcAddrValue);
        void* deviceHandle = reinterpret_cast<void*>(device);

        const std::array<const char*, 30> names = {
            "vkCreateCommandPool",
            "vkDestroyCommandPool",
            "vkAllocateCommandBuffers",
            "vkFreeCommandBuffers",
            "vkBeginCommandBuffer",
            "vkEndCommandBuffer",
            "vkResetCommandBuffer",
            "vkCreateImageView",
            "vkDestroyImageView",
            "vkCreateRenderPass",
            "vkDestroyRenderPass",
            "vkCreateFramebuffer",
            "vkDestroyFramebuffer",
            "vkCreateShaderModule",
            "vkDestroyShaderModule",
            "vkCreatePipelineLayout",
            "vkDestroyPipelineLayout",
            "vkCreateGraphicsPipelines",
            "vkDestroyPipeline",
            "vkCreateSemaphore",
            "vkDestroySemaphore",
            "vkDeviceWaitIdle",
            "vkQueueSubmit",
            "vkCmdBeginRenderPass",
            "vkCmdEndRenderPass",
            "vkCmdSetViewport",
            "vkCmdSetScissor",
            "vkCmdBindPipeline",
            "vkCmdDraw",
            "vkCmdClearAttachments"
        };

        std::ostringstream oss;
        oss << "[Minimap] Vulkan renderer function offsets"
            << " | table=" << Hex(table)
            << " | device=" << Hex(device)
            << " | getDeviceProcAddr=" << Hex(getDeviceProcAddrValue)
            << "@" << Hex(static_cast<uintptr_t>(VULKAN_TABLE_GET_DEVICE_PROC_ADDR_OFFSET));

        int inLine = 0;
        for (const char* name : names)
        {
            if (inLine == 4)
            {
                Log(oss.str());
                oss.str("");
                oss.clear();
                oss << "[Minimap] Vulkan renderer function offsets";
                inLine = 0;
            }

            AppendFunctionLookup(oss, table, getDeviceProcAddr, deviceHandle, name);
            ++inLine;
        }

        if (inLine != 0)
            Log(oss.str());

        g_vulkanFunctionOffsetsLogged = true;
    }

    bool IsExecutableAddress(uintptr_t address)
    {
        if (address == 0)
            return false;

        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) != sizeof(info))
            return false;

        if (info.State != MEM_COMMIT)
            return false;

        const DWORD protect = info.Protect & 0xFF;
        return protect == PAGE_EXECUTE ||
            protect == PAGE_EXECUTE_READ ||
            protect == PAGE_EXECUTE_READWRITE ||
            protect == PAGE_EXECUTE_WRITECOPY;
    }

    GetDeviceProcAddrFn GetExportedVulkanGetDeviceProcAddr()
    {
        HMODULE vulkanModule = GetModuleHandleW(L"vulkan-1.dll");
        if (vulkanModule == nullptr)
            vulkanModule = LoadLibraryW(L"vulkan-1.dll");

        if (vulkanModule == nullptr)
            return nullptr;

        auto* proc = reinterpret_cast<GetDeviceProcAddrFn>(GetProcAddress(vulkanModule, "vkGetDeviceProcAddr"));
        return IsExecutableAddress(reinterpret_cast<uintptr_t>(proc)) ? proc : nullptr;
    }

    GetDeviceProcAddrFn GetTableVulkanGetDeviceProcAddr(uintptr_t table)
    {
        uintptr_t getDeviceProcAddrValue = 0;
        if (!ReadVulkanTableFunction(table, VULKAN_TABLE_GET_DEVICE_PROC_ADDR_OFFSET, getDeviceProcAddrValue) ||
            !IsExecutableAddress(getDeviceProcAddrValue))
        {
            return nullptr;
        }

        return reinterpret_cast<GetDeviceProcAddrFn>(getDeviceProcAddrValue);
    }

    void* ResolveDeviceFunctionPtr(GetDeviceProcAddrFn getDeviceProcAddr, void* device, const char* name)
    {
        if (getDeviceProcAddr == nullptr || device == nullptr || name == nullptr)
            return nullptr;

        __try
        {
            void* pointer = getDeviceProcAddr(device, name);
            return IsExecutableAddress(reinterpret_cast<uintptr_t>(pointer)) ? pointer : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    template <typename T>
    T ResolveDeviceFunction(GetDeviceProcAddrFn getDeviceProcAddr, void* device, const char* name)
    {
        return reinterpret_cast<T>(ResolveDeviceFunctionPtr(getDeviceProcAddr, device, name));
    }

    bool LoadVulkanRendererFnsWithResolver(VulkanRendererFns& fns, void* device, GetDeviceProcAddrFn getDeviceProcAddr)
    {
        if (getDeviceProcAddr == nullptr)
            return false;

        const uintptr_t originalGetImages = g_originalGetSwapchainImages.load();
        fns.getSwapchainImages = originalGetImages != 0
            ? reinterpret_cast<GetSwapchainImagesFn>(originalGetImages)
            : ResolveDeviceFunction<GetSwapchainImagesFn>(getDeviceProcAddr, device, "vkGetSwapchainImagesKHR");

        fns.createImageView = ResolveDeviceFunction<CreateImageViewFn>(getDeviceProcAddr, device, "vkCreateImageView");
        fns.destroyImageView = ResolveDeviceFunction<DestroyImageViewFn>(getDeviceProcAddr, device, "vkDestroyImageView");
        fns.createRenderPass = ResolveDeviceFunction<CreateRenderPassFn>(getDeviceProcAddr, device, "vkCreateRenderPass");
        fns.destroyRenderPass = ResolveDeviceFunction<DestroyRenderPassFn>(getDeviceProcAddr, device, "vkDestroyRenderPass");
        fns.createFramebuffer = ResolveDeviceFunction<CreateFramebufferFn>(getDeviceProcAddr, device, "vkCreateFramebuffer");
        fns.destroyFramebuffer = ResolveDeviceFunction<DestroyFramebufferFn>(getDeviceProcAddr, device, "vkDestroyFramebuffer");
        fns.createCommandPool = ResolveDeviceFunction<CreateCommandPoolFn>(getDeviceProcAddr, device, "vkCreateCommandPool");
        fns.destroyCommandPool = ResolveDeviceFunction<DestroyCommandPoolFn>(getDeviceProcAddr, device, "vkDestroyCommandPool");
        fns.allocateCommandBuffers = ResolveDeviceFunction<AllocateCommandBuffersFn>(getDeviceProcAddr, device, "vkAllocateCommandBuffers");
        fns.resetCommandBuffer = ResolveDeviceFunction<ResetCommandBufferFn>(getDeviceProcAddr, device, "vkResetCommandBuffer");
        fns.beginCommandBuffer = ResolveDeviceFunction<BeginCommandBufferFn>(getDeviceProcAddr, device, "vkBeginCommandBuffer");
        fns.endCommandBuffer = ResolveDeviceFunction<EndCommandBufferFn>(getDeviceProcAddr, device, "vkEndCommandBuffer");
        fns.cmdBeginRenderPass = ResolveDeviceFunction<CmdBeginRenderPassFn>(getDeviceProcAddr, device, "vkCmdBeginRenderPass");
        fns.cmdEndRenderPass = ResolveDeviceFunction<CmdEndRenderPassFn>(getDeviceProcAddr, device, "vkCmdEndRenderPass");
        fns.cmdClearAttachments = ResolveDeviceFunction<CmdClearAttachmentsFn>(getDeviceProcAddr, device, "vkCmdClearAttachments");
        fns.createBuffer = ResolveDeviceFunction<CreateBufferFn>(getDeviceProcAddr, device, "vkCreateBuffer");
        fns.destroyBuffer = ResolveDeviceFunction<DestroyBufferFn>(getDeviceProcAddr, device, "vkDestroyBuffer");
        fns.getBufferMemoryRequirements = ResolveDeviceFunction<GetBufferMemoryRequirementsFn>(getDeviceProcAddr, device, "vkGetBufferMemoryRequirements");
        fns.createImage = ResolveDeviceFunction<CreateImageFn>(getDeviceProcAddr, device, "vkCreateImage");
        fns.destroyImage = ResolveDeviceFunction<DestroyImageFn>(getDeviceProcAddr, device, "vkDestroyImage");
        fns.getImageMemoryRequirements = ResolveDeviceFunction<GetImageMemoryRequirementsFn>(getDeviceProcAddr, device, "vkGetImageMemoryRequirements");
        fns.allocateMemory = ResolveDeviceFunction<AllocateMemoryFn>(getDeviceProcAddr, device, "vkAllocateMemory");
        fns.freeMemory = ResolveDeviceFunction<FreeMemoryFn>(getDeviceProcAddr, device, "vkFreeMemory");
        fns.bindBufferMemory = ResolveDeviceFunction<BindBufferMemoryFn>(getDeviceProcAddr, device, "vkBindBufferMemory");
        fns.bindImageMemory = ResolveDeviceFunction<BindImageMemoryFn>(getDeviceProcAddr, device, "vkBindImageMemory");
        fns.mapMemory = ResolveDeviceFunction<MapMemoryFn>(getDeviceProcAddr, device, "vkMapMemory");
        fns.unmapMemory = ResolveDeviceFunction<UnmapMemoryFn>(getDeviceProcAddr, device, "vkUnmapMemory");
        fns.createSampler = ResolveDeviceFunction<CreateSamplerFn>(getDeviceProcAddr, device, "vkCreateSampler");
        fns.destroySampler = ResolveDeviceFunction<DestroySamplerFn>(getDeviceProcAddr, device, "vkDestroySampler");
        fns.createShaderModule = ResolveDeviceFunction<CreateShaderModuleFn>(getDeviceProcAddr, device, "vkCreateShaderModule");
        fns.destroyShaderModule = ResolveDeviceFunction<DestroyShaderModuleFn>(getDeviceProcAddr, device, "vkDestroyShaderModule");
        fns.createDescriptorSetLayout = ResolveDeviceFunction<CreateDescriptorSetLayoutFn>(getDeviceProcAddr, device, "vkCreateDescriptorSetLayout");
        fns.destroyDescriptorSetLayout = ResolveDeviceFunction<DestroyDescriptorSetLayoutFn>(getDeviceProcAddr, device, "vkDestroyDescriptorSetLayout");
        fns.createDescriptorPool = ResolveDeviceFunction<CreateDescriptorPoolFn>(getDeviceProcAddr, device, "vkCreateDescriptorPool");
        fns.destroyDescriptorPool = ResolveDeviceFunction<DestroyDescriptorPoolFn>(getDeviceProcAddr, device, "vkDestroyDescriptorPool");
        fns.allocateDescriptorSets = ResolveDeviceFunction<AllocateDescriptorSetsFn>(getDeviceProcAddr, device, "vkAllocateDescriptorSets");
        fns.updateDescriptorSets = ResolveDeviceFunction<UpdateDescriptorSetsFn>(getDeviceProcAddr, device, "vkUpdateDescriptorSets");
        fns.createPipelineLayout = ResolveDeviceFunction<CreatePipelineLayoutFn>(getDeviceProcAddr, device, "vkCreatePipelineLayout");
        fns.destroyPipelineLayout = ResolveDeviceFunction<DestroyPipelineLayoutFn>(getDeviceProcAddr, device, "vkDestroyPipelineLayout");
        fns.createGraphicsPipelines = ResolveDeviceFunction<CreateGraphicsPipelinesFn>(getDeviceProcAddr, device, "vkCreateGraphicsPipelines");
        fns.destroyPipeline = ResolveDeviceFunction<DestroyPipelineFn>(getDeviceProcAddr, device, "vkDestroyPipeline");
        fns.cmdPipelineBarrier = ResolveDeviceFunction<CmdPipelineBarrierFn>(getDeviceProcAddr, device, "vkCmdPipelineBarrier");
        fns.cmdCopyBufferToImage = ResolveDeviceFunction<CmdCopyBufferToImageFn>(getDeviceProcAddr, device, "vkCmdCopyBufferToImage");
        fns.cmdBindPipeline = ResolveDeviceFunction<CmdBindPipelineFn>(getDeviceProcAddr, device, "vkCmdBindPipeline");
        fns.cmdBindDescriptorSets = ResolveDeviceFunction<CmdBindDescriptorSetsFn>(getDeviceProcAddr, device, "vkCmdBindDescriptorSets");
        fns.cmdPushConstants = ResolveDeviceFunction<CmdPushConstantsFn>(getDeviceProcAddr, device, "vkCmdPushConstants");
        fns.cmdDraw = ResolveDeviceFunction<CmdDrawFn>(getDeviceProcAddr, device, "vkCmdDraw");
        fns.createSemaphore = ResolveDeviceFunction<CreateSemaphoreFn>(getDeviceProcAddr, device, "vkCreateSemaphore");
        fns.createFence = ResolveDeviceFunction<CreateFenceFn>(getDeviceProcAddr, device, "vkCreateFence");
        fns.destroyFence = ResolveDeviceFunction<DestroyFenceFn>(getDeviceProcAddr, device, "vkDestroyFence");
        fns.waitForFences = ResolveDeviceFunction<WaitForFencesFn>(getDeviceProcAddr, device, "vkWaitForFences");
        fns.resetFences = ResolveDeviceFunction<ResetFencesFn>(getDeviceProcAddr, device, "vkResetFences");
        fns.destroySemaphore = ResolveDeviceFunction<DestroySemaphoreFn>(getDeviceProcAddr, device, "vkDestroySemaphore");
        fns.queueSubmit = ResolveDeviceFunction<QueueSubmitFn>(getDeviceProcAddr, device, "vkQueueSubmit");
        fns.deviceWaitIdle = ResolveDeviceFunction<DeviceWaitIdleFn>(getDeviceProcAddr, device, "vkDeviceWaitIdle");
        return fns.Ready();
    }

    bool TryLoadVulkanRendererFns(VulkanRendererFns& fns, void* device)
    {
        VulkanRendererFns tableFns{};
        GetDeviceProcAddrFn tableGetDeviceProcAddr = GetTableVulkanGetDeviceProcAddr(g_vulkanDeviceTable.load());
        if (LoadVulkanRendererFnsWithResolver(tableFns, device, tableGetDeviceProcAddr))
        {
            fns = tableFns;
            return true;
        }

        VulkanRendererFns exportedFns{};
        GetDeviceProcAddrFn exportedGetDeviceProcAddr = GetExportedVulkanGetDeviceProcAddr();
        if (exportedGetDeviceProcAddr != tableGetDeviceProcAddr &&
            LoadVulkanRendererFnsWithResolver(exportedFns, device, exportedGetDeviceProcAddr))
        {
            fns = exportedFns;
            LogRendererThrottled("[Minimap] Vulkan device functions resolved via vulkan-1.dll export fallback");
            return true;
        }

        return false;
    }

    void LogRendererThrottled(const std::string& message)
    {
        const DWORD now = GetTickCount();
        if (now - g_lastVulkanRendererLogTick < 5000)
            return;

        g_lastVulkanRendererLogTick = now;
        Log(message);
    }

    DWORD g_lastDrawGateLogTick = 0;

    void LogDrawGateThrottled()
    {
        if (!g_debugLoggingEnabled.load())
            return;

        const DWORD now = GetTickCount();
        if (now - g_lastDrawGateLogTick < 5000)
            return;

        g_lastDrawGateLogTick = now;

        CURSORINFO cursorInfo{};
        cursorInfo.cbSize = sizeof(cursorInfo);
        const bool cursorShowing = GetCursorInfo(&cursorInfo) && (cursorInfo.flags & CURSOR_SHOWING) != 0;

        bool positionValid = false;
        DWORD positionAgeMs = 0;
        {
            std::lock_guard<std::mutex> lock(g_playerPositionMutex);
            positionValid = g_playerPosition.valid;
            if (positionValid)
                positionAgeMs = now - g_playerPosition.lastUpdateTick;
        }

        std::ostringstream oss;
        oss << "[Minimap] draw skipped"
            << " | visible=" << (g_minimapVisible.load() ? "on" : "off")
            << " | cursor_showing=" << (cursorShowing ? "yes" : "no")
            << " | position_valid=" << (positionValid ? "yes" : "no")
            << " | position_age_ms=" << positionAgeMs
            << " | stale_after_ms=" << WORLD_DATA_STALE_MS;
        Log(oss.str());
    }

    std::string JoinPath(const std::string& base, const std::string& name);
    std::string GetExecutableDirectory();
    void EnsureMinimapFrameLoaded();
    void EnsureRealMapLoaded();
    void ReleaseRealMapCpuCopy();
    void EnsureMinimapIconsLoaded();
    // Screen-space rotation direction of embervale_minimap_frame.vert.spv relative to a
    // clockwise (y-down) rotation; verified in the SwiftShader harness.
    constexpr float GPU_SPRITE_ROTATION_SIGN = -1.0f;

    bool TryReadBinaryFile(const std::string& path, std::vector<std::uint8_t>& bytes)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file)
            return false;

        const std::streamoff size = file.tellg();
        if (size <= 0 || size > 1024 * 1024)
            return false;

        file.seekg(0, std::ios::beg);
        bytes.resize(static_cast<std::size_t>(size));
        return file.read(reinterpret_cast<char*>(bytes.data()), bytes.size()).good();
    }

    void AddFrameShaderCandidates(std::vector<std::string>& candidates, const std::string& directory, const char* name)
    {
        candidates.push_back(JoinPath(directory, name));
    }

    bool TryReadFrameShader(const char* name, std::vector<std::uint8_t>& bytes)
    {
        std::vector<std::string> candidates;
        if (g_modContext != nullptr && !g_modContext->shroudtopia.mod_folder.empty())
        {
            const std::string modRoot = JoinPath(g_modContext->shroudtopia.mod_folder, "minimap_mod");
            AddFrameShaderCandidates(candidates, modRoot, name);
            AddFrameShaderCandidates(candidates, JoinPath(modRoot, "assets"), name);
        }

        const std::string gameDir = GetExecutableDirectory();
        if (!gameDir.empty())
        {
            const std::string installedModRoot = JoinPath(JoinPath(JoinPath(gameDir, "mods"), "minimap_mod"), "");
            AddFrameShaderCandidates(candidates, installedModRoot, name);
            AddFrameShaderCandidates(candidates, JoinPath(installedModRoot, "assets"), name);
        }

        AddFrameShaderCandidates(candidates, "", name);

        for (const std::string& path : candidates)
        {
            if (TryReadBinaryFile(path, bytes))
                return true;
        }

        return false;
    }

    bool TryCreateShaderModule(VulkanMinimapRenderer& renderer, const std::vector<std::uint8_t>& bytes, void** shaderModule)
    {
        *shaderModule = nullptr;
        if (bytes.empty() || (bytes.size() % sizeof(std::uint32_t)) != 0)
            return false;

        std::vector<std::uint32_t> words(bytes.size() / sizeof(std::uint32_t));
        std::memcpy(words.data(), bytes.data(), bytes.size());

        VkShaderModuleCreateInfo shaderInfo{};
        shaderInfo.codeSize = bytes.size();
        shaderInfo.pCode = words.data();
        return renderer.fns.createShaderModule(
            reinterpret_cast<void*>(renderer.device),
            &shaderInfo,
            nullptr,
            shaderModule) == VK_SUCCESS && *shaderModule != nullptr;
    }

    bool TryAllocateMemoryByTrial(VulkanMinimapRenderer& renderer, const VkMemoryRequirements& requirements, bool requireMappable, void** memory, void** mapped)
    {
        *memory = nullptr;
        if (mapped != nullptr)
            *mapped = nullptr;

        void* device = reinterpret_cast<void*>(renderer.device);
        for (std::uint32_t index = 0; index < 32; ++index)
        {
            if ((requirements.memoryTypeBits & (1u << index)) == 0)
                continue;

            VkMemoryAllocateInfo alloc{};
            alloc.allocationSize = requirements.size;
            alloc.memoryTypeIndex = index;

            void* candidate = nullptr;
            if (renderer.fns.allocateMemory(device, &alloc, nullptr, &candidate) != VK_SUCCESS || candidate == nullptr)
                continue;

            void* mappedCandidate = nullptr;
            if (requireMappable)
            {
                if (renderer.fns.mapMemory(device, candidate, 0, requirements.size, 0, &mappedCandidate) != VK_SUCCESS || mappedCandidate == nullptr)
                {
                    renderer.fns.freeMemory(device, candidate, nullptr);
                    continue;
                }
            }

            *memory = candidate;
            if (mapped != nullptr)
                *mapped = mappedCandidate;
            return true;
        }

        return false;
    }

    bool TryCreateTexturedFrameResourcesLocked(VulkanMinimapRenderer& renderer)
    {
        if (renderer.frameTextureReady)
            return true;
        if (!renderer.fns.TextureReady())
            return false;

        EnsureMinimapFrameLoaded();

        std::vector<std::uint8_t> rgba;
        int textureWidth = 0;
        int textureHeight = 0;
        {
            std::lock_guard<std::mutex> lock(g_minimapFrameMutex);
            if (!g_minimapFrame.loaded || g_minimapFrame.rgba.empty() || g_minimapFrame.width <= 0 || g_minimapFrame.height <= 0)
                return false;

            rgba = g_minimapFrame.rgba;
            textureWidth = g_minimapFrame.width;
            textureHeight = g_minimapFrame.height;
        }

        void* device = reinterpret_cast<void*>(renderer.device);
        const std::uint64_t textureBytes = static_cast<std::uint64_t>(rgba.size());

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.size = textureBytes;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        void* stagingBuffer = nullptr;
        if (renderer.fns.createBuffer(device, &bufferInfo, nullptr, &stagingBuffer) != VK_SUCCESS || stagingBuffer == nullptr)
            return false;

        VkMemoryRequirements bufferRequirements{};
        renderer.fns.getBufferMemoryRequirements(device, stagingBuffer, &bufferRequirements);
        void* stagingMemory = nullptr;
        void* mapped = nullptr;
        if (!TryAllocateMemoryByTrial(renderer, bufferRequirements, true, &stagingMemory, &mapped))
        {
            renderer.fns.destroyBuffer(device, stagingBuffer, nullptr);
            return false;
        }
        std::memcpy(mapped, rgba.data(), rgba.size());
        renderer.fns.unmapMemory(device, stagingMemory);
        if (renderer.fns.bindBufferMemory(device, stagingBuffer, stagingMemory, 0) != VK_SUCCESS)
        {
            renderer.fns.freeMemory(device, stagingMemory, nullptr);
            renderer.fns.destroyBuffer(device, stagingBuffer, nullptr);
            return false;
        }

        VkImageCreateInfo imageInfo{};
        imageInfo.extent.width = static_cast<std::uint32_t>(textureWidth);
        imageInfo.extent.height = static_cast<std::uint32_t>(textureHeight);
        imageInfo.extent.depth = 1;
        void* textureImage = nullptr;
        if (renderer.fns.createImage(device, &imageInfo, nullptr, &textureImage) != VK_SUCCESS || textureImage == nullptr)
        {
            renderer.fns.freeMemory(device, stagingMemory, nullptr);
            renderer.fns.destroyBuffer(device, stagingBuffer, nullptr);
            return false;
        }

        VkMemoryRequirements imageRequirements{};
        renderer.fns.getImageMemoryRequirements(device, textureImage, &imageRequirements);
        void* textureMemory = nullptr;
        if (!TryAllocateMemoryByTrial(renderer, imageRequirements, false, &textureMemory, nullptr) ||
            renderer.fns.bindImageMemory(device, textureImage, textureMemory, 0) != VK_SUCCESS)
        {
            if (textureMemory != nullptr)
                renderer.fns.freeMemory(device, textureMemory, nullptr);
            renderer.fns.destroyImage(device, textureImage, nullptr);
            renderer.fns.freeMemory(device, stagingMemory, nullptr);
            renderer.fns.destroyBuffer(device, stagingBuffer, nullptr);
            return false;
        }

        VkImageViewCreateInfo textureViewInfo{};
        textureViewInfo.image = textureImage;
        textureViewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        textureViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        textureViewInfo.subresourceRange.levelCount = 1;
        textureViewInfo.subresourceRange.layerCount = 1;
        void* textureImageView = nullptr;
        if (renderer.fns.createImageView(device, &textureViewInfo, nullptr, &textureImageView) != VK_SUCCESS || textureImageView == nullptr)
            return false;

        VkSamplerCreateInfo samplerInfo{};
        void* sampler = nullptr;
        if (renderer.fns.createSampler(device, &samplerInfo, nullptr, &sampler) != VK_SUCCESS || sampler == nullptr)
            return false;

        VkDescriptorSetLayoutBinding binding{};
        VkDescriptorSetLayoutCreateInfo setLayoutInfo{};
        setLayoutInfo.bindingCount = 1;
        setLayoutInfo.pBindings = &binding;
        void* setLayout = nullptr;
        if (renderer.fns.createDescriptorSetLayout(device, &setLayoutInfo, nullptr, &setLayout) != VK_SUCCESS || setLayout == nullptr)
            return false;

        VkDescriptorPoolSize poolSize{};
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        void* descriptorPool = nullptr;
        if (renderer.fns.createDescriptorPool(device, &poolInfo, nullptr, &descriptorPool) != VK_SUCCESS || descriptorPool == nullptr)
            return false;

        void* descriptorSet = nullptr;
        void* setLayoutForAlloc = setLayout;
        VkDescriptorSetAllocateInfo setAlloc{};
        setAlloc.descriptorPool = descriptorPool;
        setAlloc.descriptorSetCount = 1;
        setAlloc.pSetLayouts = &setLayoutForAlloc;
        if (renderer.fns.allocateDescriptorSets(device, &setAlloc, &descriptorSet) != VK_SUCCESS || descriptorSet == nullptr)
            return false;

        VkDescriptorImageInfo imageDescriptor{};
        imageDescriptor.sampler = sampler;
        imageDescriptor.imageView = textureImageView;
        imageDescriptor.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet write{};
        write.dstSet = descriptorSet;
        write.descriptorCount = 1;
        write.pImageInfo = &imageDescriptor;
        renderer.fns.updateDescriptorSets(device, 1, &write, 0, nullptr);

        std::vector<std::uint8_t> vertexShader;
        std::vector<std::uint8_t> fragmentShader;
        if (!TryReadFrameShader("embervale_minimap_frame.vert.spv", vertexShader) ||
            !TryReadFrameShader("embervale_minimap_frame.frag.spv", fragmentShader))
        {
            return false;
        }

        void* vertexModule = nullptr;
        void* fragmentModule = nullptr;
        if (!TryCreateShaderModule(renderer, vertexShader, &vertexModule) ||
            !TryCreateShaderModule(renderer, fragmentShader, &fragmentModule))
        {
            if (vertexModule != nullptr)
                renderer.fns.destroyShaderModule(device, vertexModule, nullptr);
            if (fragmentModule != nullptr)
                renderer.fns.destroyShaderModule(device, fragmentModule, nullptr);
            return false;
        }

        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pushRange.size = sizeof(float) * 8;
        void* setLayoutForPipeline = setLayout;
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &setLayoutForPipeline;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &pushRange;
        void* pipelineLayout = nullptr;
        if (renderer.fns.createPipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS || pipelineLayout == nullptr)
        {
            renderer.fns.destroyShaderModule(device, vertexModule, nullptr);
            renderer.fns.destroyShaderModule(device, fragmentModule, nullptr);
            return false;
        }

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertexModule;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragmentModule;

        VkPipelineVertexInputStateCreateInfo vertexInput{};
        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        VkViewport viewport{};
        viewport.width = static_cast<float>(renderer.width);
        viewport.height = static_cast<float>(renderer.height);
        VkRect2D scissor{};
        scissor.extent.width = renderer.width;
        scissor.extent.height = renderer.height;
        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.viewportCount = 1;
        viewportState.pViewports = &viewport;
        viewportState.scissorCount = 1;
        viewportState.pScissors = &scissor;
        VkPipelineRasterizationStateCreateInfo raster{};
        VkPipelineMultisampleStateCreateInfo multisample{};
        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.blendEnable = 1;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        VkPipelineColorBlendStateCreateInfo blend{};
        blend.attachmentCount = 1;
        blend.pAttachments = &blendAttachment;

        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = stages;
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &raster;
        pipelineInfo.pMultisampleState = &multisample;
        pipelineInfo.pColorBlendState = &blend;
        pipelineInfo.layout = pipelineLayout;
        pipelineInfo.renderPass = reinterpret_cast<void*>(renderer.renderPass);

        void* pipeline = nullptr;
        const bool pipelineOk = renderer.fns.createGraphicsPipelines(device, nullptr, 1, &pipelineInfo, nullptr, &pipeline) == VK_SUCCESS && pipeline != nullptr;
        renderer.fns.destroyShaderModule(device, vertexModule, nullptr);
        renderer.fns.destroyShaderModule(device, fragmentModule, nullptr);
        if (!pipelineOk)
            return false;

        renderer.frameStagingBuffer = reinterpret_cast<uintptr_t>(stagingBuffer);
        renderer.frameStagingMemory = reinterpret_cast<uintptr_t>(stagingMemory);
        renderer.frameTextureImage = reinterpret_cast<uintptr_t>(textureImage);
        renderer.frameTextureMemory = reinterpret_cast<uintptr_t>(textureMemory);
        renderer.frameTextureImageView = reinterpret_cast<uintptr_t>(textureImageView);
        renderer.frameSampler = reinterpret_cast<uintptr_t>(sampler);
        renderer.frameDescriptorSetLayout = reinterpret_cast<uintptr_t>(setLayout);
        renderer.frameDescriptorPool = reinterpret_cast<uintptr_t>(descriptorPool);
        renderer.frameDescriptorSet = reinterpret_cast<uintptr_t>(descriptorSet);
        renderer.framePipelineLayout = reinterpret_cast<uintptr_t>(pipelineLayout);
        renderer.framePipeline = reinterpret_cast<uintptr_t>(pipeline);
        renderer.frameTextureWidth = static_cast<std::uint32_t>(textureWidth);
        renderer.frameTextureHeight = static_cast<std::uint32_t>(textureHeight);
        renderer.frameTextureUploadPending = true;
        renderer.frameTextureReady = true;
        return true;
    }

    // ---- BEGIN GPU MAP RENDERER ----
    // The map used to be rasterized on the CPU: one vkCmdClearAttachments rect per run
    // of same-colored 2x2 pixel blocks, colors quantized to 48 levels. That caps the
    // visible detail far below the map image itself (the minimap is magnified up to
    // ~10 screen pixels per texel at max zoom). This path uploads the map once as a
    // mipmapped texture and draws the whole window with a fragment shader: zoom,
    // trilinear filtering and an anti-aliased edge, like a regular map UI.
    // Map icons and the player marker use a second pipeline of the same kind: a
    // mipmapped sprite atlas drawn as filtered, alpha-blended quads.
    constexpr const char* MINIMAP_MAP_VERTEX_SHADER = "embervale_minimap_frame.vert.spv";
    constexpr const char* MINIMAP_MAP_FRAGMENT_SHADER = "embervale_minimap_map.frag.spv";
    constexpr const char* MINIMAP_SPRITE_FRAGMENT_SHADER = "embervale_minimap_sprite.frag.spv";
    constexpr std::uint32_t MINIMAP_MAP_PUSH_BYTES = sizeof(float) * 16;
    constexpr float MINIMAP_GPU_MAP_VIGNETTE = 0.10f;
    constexpr std::uint32_t SPRITE_ATLAS_PADDING = 8;
    constexpr std::uint32_t SPRITE_ATLAS_MAX_SIZE = 4096;
    constexpr std::uint32_t SPRITE_ATLAS_MAX_LEVELS = 5;
    // Built-in sprites generated at runtime (keys below every real marker hash).
    constexpr std::uint32_t SPRITE_KEY_PLAYER = 0xFFFFFF01u;
    constexpr std::uint32_t SPRITE_KEY_ALLY = 0xFFFFFF02u;
    constexpr std::uint32_t SPRITE_KEY_NPC = 0xFFFFFF03u;
    constexpr std::uint32_t SPRITE_KEY_WAYPOINT_RING = 0xFFFFFF04u;
    constexpr std::uint32_t SPRITE_KEY_PLAYER_DOT = 0xFFFFFF05u;   // us, with the heading off (F11)
    // One yellow silhouette per map icon: the icon's shape grown by a few pixels, drawn
    // under the icon so the waypoint highlight follows the icon's outline.
    constexpr std::uint32_t SPRITE_KEY_SILHOUETTE_BASE = 0xFFFE0000u;

    std::uint32_t ComputeMipLevelCount(std::uint32_t size)
    {
        std::uint32_t levels = 1;
        while (size > 1)
        {
            size >>= 1;
            ++levels;
        }
        return levels;
    }

    // 2x2 box filter; odd source edges reuse the last row/column.
    void DownsampleRgbaLevel(const std::uint8_t* src, std::uint32_t srcSize, std::uint8_t* dst, std::uint32_t dstSize)
    {
        const std::size_t srcStride = static_cast<std::size_t>(srcSize);
        for (std::uint32_t y = 0; y < dstSize; ++y)
        {
            const std::uint32_t y0 = MinValue(y * 2, srcSize - 1);
            const std::uint32_t y1 = MinValue(y * 2 + 1, srcSize - 1);
            for (std::uint32_t x = 0; x < dstSize; ++x)
            {
                const std::uint32_t x0 = MinValue(x * 2, srcSize - 1);
                const std::uint32_t x1 = MinValue(x * 2 + 1, srcSize - 1);
                const std::size_t a = (static_cast<std::size_t>(y0) * srcStride + x0) * 4;
                const std::size_t b = (static_cast<std::size_t>(y0) * srcStride + x1) * 4;
                const std::size_t c = (static_cast<std::size_t>(y1) * srcStride + x0) * 4;
                const std::size_t d = (static_cast<std::size_t>(y1) * srcStride + x1) * 4;
                const std::size_t o = (static_cast<std::size_t>(y) * dstSize + x) * 4;
                for (std::size_t channel = 0; channel < 4; ++channel)
                {
                    const unsigned sum =
                        static_cast<unsigned>(src[a + channel]) +
                        static_cast<unsigned>(src[b + channel]) +
                        static_cast<unsigned>(src[c + channel]) +
                        static_cast<unsigned>(src[d + channel]) + 2u;
                    dst[o + channel] = static_cast<std::uint8_t>(sum / 4u);
                }
            }
        }
    }

    void ReleaseGpuTextureStagingLocked(VulkanMinimapRenderer& renderer, GpuTexturePipeline& gp)
    {
        void* device = reinterpret_cast<void*>(renderer.device);
        if (device != nullptr)
        {
            if (gp.stagingMapped != nullptr && gp.stagingMemory != 0 && renderer.fns.unmapMemory != nullptr)
                renderer.fns.unmapMemory(device, reinterpret_cast<void*>(gp.stagingMemory));
            gp.stagingMapped = nullptr;
            if (gp.stagingBuffer != 0 && renderer.fns.destroyBuffer != nullptr)
                renderer.fns.destroyBuffer(device, reinterpret_cast<void*>(gp.stagingBuffer), nullptr);
            if (gp.stagingMemory != 0 && renderer.fns.freeMemory != nullptr)
                renderer.fns.freeMemory(device, reinterpret_cast<void*>(gp.stagingMemory), nullptr);
        }

        gp.stagingBuffer = 0;
        gp.stagingMemory = 0;
        gp.stagingReleasePending = false;
        std::vector<VkBufferImageCopy>().swap(gp.uploadRegions);
    }

    // Frees every Vulkan object of the pipeline. `attempted` is kept on purpose: a
    // failed creation is not retried until the whole renderer is rebuilt.
    void DestroyGpuTexturePipelineLocked(VulkanMinimapRenderer& renderer, GpuTexturePipeline& gp)
    {
        void* device = reinterpret_cast<void*>(renderer.device);
        const VulkanRendererFns& fns = renderer.fns;
        if (device != nullptr)
        {
            if (gp.pipeline != 0 && fns.destroyPipeline != nullptr)
                fns.destroyPipeline(device, reinterpret_cast<void*>(gp.pipeline), nullptr);
            if (gp.pipelineLayout != 0 && fns.destroyPipelineLayout != nullptr)
                fns.destroyPipelineLayout(device, reinterpret_cast<void*>(gp.pipelineLayout), nullptr);
            if (gp.descriptorPool != 0 && fns.destroyDescriptorPool != nullptr)
                fns.destroyDescriptorPool(device, reinterpret_cast<void*>(gp.descriptorPool), nullptr);
            if (gp.descriptorSetLayout != 0 && fns.destroyDescriptorSetLayout != nullptr)
                fns.destroyDescriptorSetLayout(device, reinterpret_cast<void*>(gp.descriptorSetLayout), nullptr);
            if (gp.sampler != 0 && fns.destroySampler != nullptr)
                fns.destroySampler(device, reinterpret_cast<void*>(gp.sampler), nullptr);
            if (gp.imageView != 0 && fns.destroyImageView != nullptr)
                fns.destroyImageView(device, reinterpret_cast<void*>(gp.imageView), nullptr);
            if (gp.image != 0 && fns.destroyImage != nullptr)
                fns.destroyImage(device, reinterpret_cast<void*>(gp.image), nullptr);
            if (gp.memory != 0 && fns.freeMemory != nullptr)
                fns.freeMemory(device, reinterpret_cast<void*>(gp.memory), nullptr);
        }

        ReleaseGpuTextureStagingLocked(renderer, gp);
        const bool attempted = gp.attempted;
        gp = GpuTexturePipeline{};
        gp.attempted = attempted;
    }

    // Uploads a square RGBA image (plus up to maxLevels-1 generated mips) and builds a
    // textured-quad pipeline around the given fragment shader. The pixels are copied
    // into the staging buffer here; the GPU upload is recorded later by
    // RecordGpuTextureUploadIfNeeded (outside the render pass).
    // Writes the base image plus its generated mips into a mapped staging buffer laid
    // out as `regions` describes.
    void WriteMipChainToStaging(const std::vector<VkBufferImageCopy>& regions, std::uint8_t* out, const std::uint8_t* rgba, std::uint32_t size)
    {
        std::memcpy(out, rgba, static_cast<std::size_t>(size) * size * 4);

        std::vector<std::uint8_t> previous;
        const std::uint8_t* source = rgba;
        std::uint32_t sourceSize = size;
        for (std::size_t level = 1; level < regions.size(); ++level)
        {
            const std::uint32_t dim = regions[level].imageExtent.width;
            std::vector<std::uint8_t> current(static_cast<std::size_t>(dim) * dim * 4);
            DownsampleRgbaLevel(source, sourceSize, current.data(), dim);
            std::memcpy(out + regions[level].bufferOffset, current.data(), current.size());
            previous.swap(current);
            source = previous.data();
            sourceSize = dim;
        }
    }

    bool TryCreateGpuTexturePipelineLocked(
        VulkanMinimapRenderer& renderer,
        GpuTexturePipeline& gp,
        const char* fragmentShaderName,
        const std::uint8_t* rgba,
        std::uint32_t size,
        std::uint32_t maxLevels,
        const char* label,
        bool persistentStaging = false,
        const std::vector<std::vector<std::uint8_t>>* prebuiltMips = nullptr)
    {
        const auto fail = [&renderer, &gp, label](const char* step) -> bool
        {
            DestroyGpuTexturePipelineLocked(renderer, gp);
            Log(std::string("[Minimap] GPU ") + label + " unavailable (" + step + ")");
            return false;
        };

        std::vector<std::uint8_t> vertexShader;
        std::vector<std::uint8_t> fragmentShader;
        if (!TryReadFrameShader(MINIMAP_MAP_VERTEX_SHADER, vertexShader) ||
            !TryReadFrameShader(fragmentShaderName, fragmentShader))
        {
            Log(std::string("[Minimap] GPU ") + label + " shader missing (" + fragmentShaderName + ")");
            return false;
        }

        std::uint32_t levels = MaxValue<std::uint32_t>(1, MinValue(ComputeMipLevelCount(size), maxLevels));
        if (prebuiltMips != nullptr)
        {
            if (prebuiltMips->empty())
                return false;
            levels = MinValue<std::uint32_t>(levels, static_cast<std::uint32_t>(prebuiltMips->size()));
        }
        std::vector<VkBufferImageCopy> regions(levels);
        std::uint64_t totalBytes = 0;
        for (std::uint32_t level = 0; level < levels; ++level)
        {
            const std::uint32_t dim = MaxValue<std::uint32_t>(1, size >> level);
            VkBufferImageCopy& region = regions[level];
            region.bufferOffset = totalBytes;
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.mipLevel = level;
            region.imageSubresource.layerCount = 1;
            region.imageExtent.width = dim;
            region.imageExtent.height = dim;
            region.imageExtent.depth = 1;
            totalBytes += static_cast<std::uint64_t>(dim) * dim * 4;
        }

        void* device = reinterpret_cast<void*>(renderer.device);

        // Staging buffer holding the whole mip chain.
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.size = totalBytes;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        void* stagingBuffer = nullptr;
        if (renderer.fns.createBuffer(device, &bufferInfo, nullptr, &stagingBuffer) != VK_SUCCESS || stagingBuffer == nullptr)
            return fail("staging buffer");
        gp.stagingBuffer = reinterpret_cast<uintptr_t>(stagingBuffer);

        VkMemoryRequirements bufferRequirements{};
        renderer.fns.getBufferMemoryRequirements(device, stagingBuffer, &bufferRequirements);
        void* stagingMemory = nullptr;
        void* mapped = nullptr;
        if (!TryAllocateMemoryByTrial(renderer, bufferRequirements, true, &stagingMemory, &mapped))
            return fail("staging memory");
        gp.stagingMemory = reinterpret_cast<uintptr_t>(stagingMemory);

        if (prebuiltMips != nullptr)
        {
            // The chain was generated off the render thread; this is just the copy.
            auto* out = static_cast<std::uint8_t*>(mapped);
            for (std::uint32_t level = 0; level < levels; ++level)
            {
                const std::uint32_t dim = regions[level].imageExtent.width;
                const std::size_t bytes = static_cast<std::size_t>(dim) * dim * 4;
                const std::vector<std::uint8_t>& mip = (*prebuiltMips)[level];
                if (mip.size() < bytes)
                    return fail("prebuilt mip size");
                std::memcpy(out + regions[level].bufferOffset, mip.data(), bytes);
            }
        }
        else
        {
            WriteMipChainToStaging(regions, static_cast<std::uint8_t*>(mapped), rgba, size);
        }
        if (persistentStaging)
        {
            gp.persistentStaging = true;
            gp.stagingMapped = static_cast<std::uint8_t*>(mapped);
        }
        else
        {
            renderer.fns.unmapMemory(device, stagingMemory);
        }
        if (renderer.fns.bindBufferMemory(device, stagingBuffer, stagingMemory, 0) != VK_SUCCESS)
            return fail("bind staging memory");

        // Sampled texture.
        VkImageCreateInfo imageInfo{};
        imageInfo.extent.width = size;
        imageInfo.extent.height = size;
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = levels;
        void* textureImage = nullptr;
        if (renderer.fns.createImage(device, &imageInfo, nullptr, &textureImage) != VK_SUCCESS || textureImage == nullptr)
            return fail("image");
        gp.image = reinterpret_cast<uintptr_t>(textureImage);

        VkMemoryRequirements imageRequirements{};
        renderer.fns.getImageMemoryRequirements(device, textureImage, &imageRequirements);
        void* textureMemory = nullptr;
        if (!TryAllocateMemoryByTrial(renderer, imageRequirements, false, &textureMemory, nullptr))
            return fail("image memory");
        gp.memory = reinterpret_cast<uintptr_t>(textureMemory);
        if (renderer.fns.bindImageMemory(device, textureImage, textureMemory, 0) != VK_SUCCESS)
            return fail("bind image memory");

        VkImageViewCreateInfo viewInfo{};
        viewInfo.image = textureImage;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = levels;
        viewInfo.subresourceRange.layerCount = 1;
        void* textureView = nullptr;
        if (renderer.fns.createImageView(device, &viewInfo, nullptr, &textureView) != VK_SUCCESS || textureView == nullptr)
            return fail("image view");
        gp.imageView = reinterpret_cast<uintptr_t>(textureView);

        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.maxLod = static_cast<float>(levels);
        void* sampler = nullptr;
        if (renderer.fns.createSampler(device, &samplerInfo, nullptr, &sampler) != VK_SUCCESS || sampler == nullptr)
            return fail("sampler");
        gp.sampler = reinterpret_cast<uintptr_t>(sampler);

        // Descriptor set: binding 0 = combined image sampler (fragment stage).
        VkDescriptorSetLayoutBinding binding{};
        VkDescriptorSetLayoutCreateInfo setLayoutInfo{};
        setLayoutInfo.bindingCount = 1;
        setLayoutInfo.pBindings = &binding;
        void* setLayout = nullptr;
        if (renderer.fns.createDescriptorSetLayout(device, &setLayoutInfo, nullptr, &setLayout) != VK_SUCCESS || setLayout == nullptr)
            return fail("descriptor set layout");
        gp.descriptorSetLayout = reinterpret_cast<uintptr_t>(setLayout);

        VkDescriptorPoolSize poolSize{};
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        void* descriptorPool = nullptr;
        if (renderer.fns.createDescriptorPool(device, &poolInfo, nullptr, &descriptorPool) != VK_SUCCESS || descriptorPool == nullptr)
            return fail("descriptor pool");
        gp.descriptorPool = reinterpret_cast<uintptr_t>(descriptorPool);

        void* descriptorSet = nullptr;
        void* setLayoutForAlloc = setLayout;
        VkDescriptorSetAllocateInfo setAlloc{};
        setAlloc.descriptorPool = descriptorPool;
        setAlloc.descriptorSetCount = 1;
        setAlloc.pSetLayouts = &setLayoutForAlloc;
        if (renderer.fns.allocateDescriptorSets(device, &setAlloc, &descriptorSet) != VK_SUCCESS || descriptorSet == nullptr)
            return fail("descriptor set");
        gp.descriptorSet = reinterpret_cast<uintptr_t>(descriptorSet);

        VkDescriptorImageInfo imageDescriptor{};
        imageDescriptor.sampler = sampler;
        imageDescriptor.imageView = textureView;
        imageDescriptor.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet write{};
        write.dstSet = descriptorSet;
        write.descriptorCount = 1;
        write.pImageInfo = &imageDescriptor;
        renderer.fns.updateDescriptorSets(device, 1, &write, 0, nullptr);

        void* vertexModule = nullptr;
        void* fragmentModule = nullptr;
        if (!TryCreateShaderModule(renderer, vertexShader, &vertexModule) ||
            !TryCreateShaderModule(renderer, fragmentShader, &fragmentModule))
        {
            if (vertexModule != nullptr)
                renderer.fns.destroyShaderModule(device, vertexModule, nullptr);
            if (fragmentModule != nullptr)
                renderer.fns.destroyShaderModule(device, fragmentModule, nullptr);
            return fail("shader modules");
        }

        // One range shared by both stages: the (reused) frame vertex shader reads the
        // quad rect + rotation at 0..31, the fragment shaders read 32..63.
        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushRange.size = MINIMAP_MAP_PUSH_BYTES;
        void* setLayoutForPipeline = setLayout;
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &setLayoutForPipeline;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &pushRange;
        void* pipelineLayout = nullptr;
        if (renderer.fns.createPipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS || pipelineLayout == nullptr)
        {
            renderer.fns.destroyShaderModule(device, vertexModule, nullptr);
            renderer.fns.destroyShaderModule(device, fragmentModule, nullptr);
            return fail("pipeline layout");
        }
        gp.pipelineLayout = reinterpret_cast<uintptr_t>(pipelineLayout);

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertexModule;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragmentModule;

        VkPipelineVertexInputStateCreateInfo vertexInput{};
        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        VkViewport viewport{};
        viewport.width = static_cast<float>(renderer.width);
        viewport.height = static_cast<float>(renderer.height);
        VkRect2D scissor{};
        scissor.extent.width = renderer.width;
        scissor.extent.height = renderer.height;
        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.viewportCount = 1;
        viewportState.pViewports = &viewport;
        viewportState.scissorCount = 1;
        viewportState.pScissors = &scissor;
        VkPipelineRasterizationStateCreateInfo raster{};
        VkPipelineMultisampleStateCreateInfo multisample{};
        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.blendEnable = 1;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        VkPipelineColorBlendStateCreateInfo blend{};
        blend.attachmentCount = 1;
        blend.pAttachments = &blendAttachment;

        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = stages;
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &raster;
        pipelineInfo.pMultisampleState = &multisample;
        pipelineInfo.pColorBlendState = &blend;
        pipelineInfo.layout = pipelineLayout;
        pipelineInfo.renderPass = reinterpret_cast<void*>(renderer.renderPass);

        void* pipeline = nullptr;
        const bool pipelineOk = renderer.fns.createGraphicsPipelines(device, nullptr, 1, &pipelineInfo, nullptr, &pipeline) == VK_SUCCESS && pipeline != nullptr;
        renderer.fns.destroyShaderModule(device, vertexModule, nullptr);
        renderer.fns.destroyShaderModule(device, fragmentModule, nullptr);
        if (!pipelineOk)
            return fail("pipeline");
        gp.pipeline = reinterpret_cast<uintptr_t>(pipeline);

        gp.uploadRegions = std::move(regions);
        gp.size = size;
        gp.mipLevels = levels;
        gp.uploadPending = true;
        gp.stagingReleasePending = false;
        gp.stagingBusy = persistentStaging;
        gp.ready = true;
        gp.uploadBytes = totalBytes;
        return true;
    }

    bool CanStartGpuPipelineLocked(const VulkanMinimapRenderer& renderer)
    {
        return renderer.fns.TextureReady() && renderer.device != 0 && renderer.renderPass != 0 &&
            renderer.width != 0 && renderer.height != 0;
    }

    bool TryCreateGpuMapResourcesLocked(VulkanMinimapRenderer& renderer)
    {
        GpuTexturePipeline& gp = renderer.mapGpu;
        if (gp.ready)
            return true;
        if (gp.attempted)
            return false;
        gp.attempted = true;

        if (!CanStartGpuPipelineLocked(renderer))
            return false;

        EnsureRealMapLoaded();
        std::lock_guard<std::mutex> mapLock(g_realMapMutex);
        if (!g_realMap.loaded || g_realMap.rgba.empty() || g_realMap.width <= 0 || g_realMap.width != g_realMap.height)
            return false;

        const std::uint32_t size = static_cast<std::uint32_t>(g_realMap.width);
        if (g_realMap.rgba.size() < static_cast<std::size_t>(size) * size * 4)
            return false;

        if (!TryCreateGpuTexturePipelineLocked(renderer, gp, MINIMAP_MAP_FRAGMENT_SHADER, g_realMap.rgba.data(), size, 32, "map"))
        {
            Log("[Minimap] using CPU map fallback");
            return false;
        }

        std::ostringstream oss;
        oss << "[Minimap] GPU map renderer ready"
            << " | size=" << size << "x" << size
            << " | mip_levels=" << gp.mipLevels
            << " | upload_bytes=" << gp.uploadBytes
            << " | path=" << g_realMap.path;
        Log(oss.str());
        return true;
    }

    // ---- sprites ----

    struct SpriteImage
    {
        std::uint32_t key = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::vector<std::uint8_t> rgba;
    };

    float SmoothCoverage(float signedDistance)
    {
        // signedDistance in pixels, negative inside: 1px anti-aliased edge.
        return ClampValue(0.5f - signedDistance, 0.0f, 1.0f);
    }

    void BlendSpritePixel(std::uint8_t* px, float red, float green, float blue, float alpha)
    {
        const float dstAlpha = static_cast<float>(px[3]) / 255.0f;
        const float outAlpha = alpha + dstAlpha * (1.0f - alpha);
        if (outAlpha <= 0.0f)
            return;
        const auto mix = [&](int channel, float value)
        {
            const float dst = static_cast<float>(px[channel]) / 255.0f;
            const float result = (value * alpha + dst * dstAlpha * (1.0f - alpha)) / outAlpha;
            px[channel] = static_cast<std::uint8_t>(ClampValue(result, 0.0f, 1.0f) * 255.0f + 0.5f);
        };
        mix(0, red);
        mix(1, green);
        mix(2, blue);
        px[3] = static_cast<std::uint8_t>(ClampValue(outAlpha, 0.0f, 1.0f) * 255.0f + 0.5f);
    }

    // Lime-green triangle pointing up (north), thin dark outline.
    SpriteImage BuildPlayerSprite()
    {
        constexpr std::uint32_t S = 96;
        SpriteImage image{ SPRITE_KEY_PLAYER, S, S, std::vector<std::uint8_t>(S * S * 4, 0) };
        const float tipX = 48.0f, tipY = 8.0f;
        const float leftX = 18.0f, leftY = 86.0f;
        const float rightX = 78.0f, rightY = 86.0f;
        const auto edgeDistance = [](float px, float py, float ax, float ay, float bx, float by)
        {
            // signed distance to the line a->b (positive on the triangle's inner side for
            // the tip -> left -> right winding used below)
            const float ex = bx - ax;
            const float ey = by - ay;
            const float length = std::sqrt(ex * ex + ey * ey);
            return ((px - ax) * ey - (py - ay) * ex) / length;
        };
        for (std::uint32_t y = 0; y < S; ++y)
        {
            for (std::uint32_t x = 0; x < S; ++x)
            {
                const float px = static_cast<float>(x) + 0.5f;
                const float py = static_cast<float>(y) + 0.5f;
                // Inside, all three edge distances are positive; d < 0 inside.
                const float d = -MinValue(
                    MinValue(edgeDistance(px, py, tipX, tipY, leftX, leftY), edgeDistance(px, py, leftX, leftY, rightX, rightY)),
                    edgeDistance(px, py, rightX, rightY, tipX, tipY));
                std::uint8_t* out = image.rgba.data() + (static_cast<std::size_t>(y) * S + x) * 4;
                // outline (6px at 96px = ~1.3px on screen), then fill
                BlendSpritePixel(out, 0.06f, 0.16f, 0.04f, SmoothCoverage(d - 6.0f) * 0.95f);
                BlendSpritePixel(out, 0.62f, 0.95f, 0.22f, SmoothCoverage(d));
            }
        }
        return image;
    }

    // Dot with a thin dark rim (sky blue: other players, yellow: NPCs).
    int IconSilhouetteRadius(std::uint32_t width, std::uint32_t height)
    {
        return ClampValue(static_cast<int>(MaxValue(width, height)) / 14, 4, 12);
    }

    // The silhouette canvas is padded by this much on every side, so the outline of an
    // icon that fills its texture (the waypoint diamond) is not cut off at the edges.
    std::uint32_t IconSilhouettePadding(std::uint32_t width, std::uint32_t height)
    {
        return static_cast<std::uint32_t>(IconSilhouetteRadius(width, height)) + 2;
    }

    SpriteImage BuildIconSilhouette(std::uint32_t key, const std::vector<std::uint8_t>& rgba, std::uint32_t sourceWidth, std::uint32_t sourceHeight)
    {
        const std::uint32_t pad = IconSilhouettePadding(sourceWidth, sourceHeight);
        const std::uint32_t width = sourceWidth + pad * 2;
        const std::uint32_t height = sourceHeight + pad * 2;
        SpriteImage image{ key, width, height, std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4, 0) };
        if (rgba.size() < static_cast<std::size_t>(sourceWidth) * sourceHeight * 4 || sourceWidth == 0 || sourceHeight == 0)
            return image;

        // Chamfer (3-4) distance to the nearest opaque pixel: a round outline that keeps
        // the icon's shape (a box dilation turned the diamond into an octagon).
        const float radius = static_cast<float>(IconSilhouetteRadius(sourceWidth, sourceHeight));
        constexpr int UNREACHED = 1 << 20;   // (FAR/NEAR are windows.h macros)
        std::vector<int> distance(static_cast<std::size_t>(width) * height, UNREACHED);
        for (std::uint32_t y = 0; y < sourceHeight; ++y)
        {
            for (std::uint32_t x = 0; x < sourceWidth; ++x)
            {
                if (rgba[(static_cast<std::size_t>(y) * sourceWidth + x) * 4 + 3] >= 40)
                    distance[static_cast<std::size_t>(y + pad) * width + (x + pad)] = 0;
            }
        }
        const auto at = [&](std::uint32_t x, std::uint32_t y) -> int& { return distance[static_cast<std::size_t>(y) * width + x]; };
        for (std::uint32_t y = 0; y < height; ++y)
        {
            for (std::uint32_t x = 0; x < width; ++x)
            {
                int best = at(x, y);
                if (x > 0) best = MinValue(best, at(x - 1, y) + 3);
                if (y > 0)
                {
                    best = MinValue(best, at(x, y - 1) + 3);
                    if (x > 0) best = MinValue(best, at(x - 1, y - 1) + 4);
                    if (x + 1 < width) best = MinValue(best, at(x + 1, y - 1) + 4);
                }
                at(x, y) = best;
            }
        }
        for (std::uint32_t yy = height; yy-- > 0;)
        {
            for (std::uint32_t xx = width; xx-- > 0;)
            {
                int best = at(xx, yy);
                if (xx + 1 < width) best = MinValue(best, at(xx + 1, yy) + 3);
                if (yy + 1 < height)
                {
                    best = MinValue(best, at(xx, yy + 1) + 3);
                    if (xx + 1 < width) best = MinValue(best, at(xx + 1, yy + 1) + 4);
                    if (xx > 0) best = MinValue(best, at(xx - 1, yy + 1) + 4);
                }
                at(xx, yy) = best;
            }
        }

        for (std::uint32_t y = 0; y < height; ++y)
        {
            for (std::uint32_t x = 0; x < width; ++x)
            {
                const float d = static_cast<float>(at(x, y)) / 3.0f;
                const float coverage = ClampValue(radius + 0.5f - d, 0.0f, 1.0f);
                if (coverage <= 0.0f)
                    continue;
                const std::size_t index = static_cast<std::size_t>(y) * width + x;
                image.rgba[index * 4 + 0] = 255;
                image.rgba[index * 4 + 1] = 205;
                image.rgba[index * 4 + 2] = 20;
                image.rgba[index * 4 + 3] = static_cast<std::uint8_t>(coverage * 255.0f + 0.5f);
            }
        }
        return image;
    }

    // The game marks the active waypoint with a yellow ring around the icon that sits
    // there; the ring is drawn on its own when no icon shares the spot.
    SpriteImage BuildWaypointRingSprite()
    {
        constexpr std::uint32_t S = 96;
        SpriteImage image{ SPRITE_KEY_WAYPOINT_RING, S, S, std::vector<std::uint8_t>(S * S * 4, 0) };
        constexpr float CENTER = 48.0f;
        constexpr float RADIUS = 30.0f;
        for (std::uint32_t y = 0; y < S; ++y)
        {
            for (std::uint32_t x = 0; x < S; ++x)
            {
                const float dx = static_cast<float>(x) + 0.5f - CENTER;
                const float dy = static_cast<float>(y) + 0.5f - CENTER;
                // Filled diamond: inside is where |dx| + |dy| stays under the radius.
                const float diamond = (std::fabs(dx) + std::fabs(dy) - RADIUS) * 0.7071f;
                std::uint8_t* out = image.rgba.data() + (static_cast<std::size_t>(y) * S + x) * 4;
                BlendSpritePixel(out, 0.10f, 0.08f, 0.02f, SmoothCoverage(diamond - 2.5f) * 0.85f);
                BlendSpritePixel(out, 1.00f, 0.80f, 0.10f, SmoothCoverage(diamond));
            }
        }
        return image;
    }

    SpriteImage BuildDotSprite(std::uint32_t key, float red, float green, float blue, float rimRed, float rimGreen, float rimBlue)
    {
        constexpr std::uint32_t S = 64;
        SpriteImage image{ key, S, S, std::vector<std::uint8_t>(S * S * 4, 0) };
        for (std::uint32_t y = 0; y < S; ++y)
        {
            for (std::uint32_t x = 0; x < S; ++x)
            {
                const float dx = static_cast<float>(x) + 0.5f - 32.0f;
                const float dy = static_cast<float>(y) + 0.5f - 32.0f;
                const float d = std::sqrt(dx * dx + dy * dy) - 22.0f;
                std::uint8_t* out = image.rgba.data() + (static_cast<std::size_t>(y) * S + x) * 4;
                BlendSpritePixel(out, rimRed, rimGreen, rimBlue, SmoothCoverage(d - 6.0f) * 0.9f);
                BlendSpritePixel(out, red, green, blue, SmoothCoverage(d));
            }
        }
        return image;
    }

    // Transparent texels take the color of an opaque neighbour so filtered/mipmapped
    // sampling does not pull dark fringes into icon edges.
    void BleedTransparentColor(std::vector<std::uint8_t>& rgba, std::uint32_t width, std::uint32_t height)
    {
        std::vector<std::uint8_t> filled(static_cast<std::size_t>(width) * height, 0);
        for (std::size_t i = 0; i < filled.size(); ++i)
            filled[i] = rgba[i * 4 + 3] != 0 ? 1 : 0;

        for (int pass = 0; pass < 8; ++pass)
        {
            std::vector<std::uint8_t> next = filled;
            bool changed = false;
            for (std::uint32_t y = 0; y < height; ++y)
            {
                for (std::uint32_t x = 0; x < width; ++x)
                {
                    const std::size_t index = static_cast<std::size_t>(y) * width + x;
                    if (filled[index])
                        continue;
                    unsigned r = 0, g = 0, b = 0, n = 0;
                    for (int oy = -1; oy <= 1; ++oy)
                    {
                        for (int ox = -1; ox <= 1; ++ox)
                        {
                            const int nx = static_cast<int>(x) + ox;
                            const int ny = static_cast<int>(y) + oy;
                            if (nx < 0 || ny < 0 || nx >= static_cast<int>(width) || ny >= static_cast<int>(height))
                                continue;
                            const std::size_t ni = static_cast<std::size_t>(ny) * width + static_cast<std::size_t>(nx);
                            if (!filled[ni])
                                continue;
                            r += rgba[ni * 4];
                            g += rgba[ni * 4 + 1];
                            b += rgba[ni * 4 + 2];
                            ++n;
                        }
                    }
                    if (n == 0)
                        continue;
                    rgba[index * 4] = static_cast<std::uint8_t>(r / n);
                    rgba[index * 4 + 1] = static_cast<std::uint8_t>(g / n);
                    rgba[index * 4 + 2] = static_cast<std::uint8_t>(b / n);
                    next[index] = 1;
                    changed = true;
                }
            }
            filled.swap(next);
            if (!changed)
                break;
        }
    }

    // ---- PLAYER NAME LABELS ----
    // Names are drawn as sprites: the text is rasterised once with GDI (so Korean,
    // Chinese and Japanese names work), then packed into the sprite atlas. A new name
    // asks for an atlas rebuild, which happens on the render thread.
    constexpr std::uint32_t SPRITE_KEY_NAME_BASE = 0xFFFFFE00u;
    constexpr std::size_t MAX_NAME_SPRITES = 24;
    constexpr std::uint32_t NAME_SPRITE_MAX_WIDTH = 128;
    int NameSpriteFontHeight()
    {
        return ClampValue(g_minimapLabelFontSize.load(), 8, 40) + 1;
    }

    struct NameSprite
    {
        std::uint32_t key = 0;
        std::string text;
    };

    std::mutex g_nameSpriteMutex;
    std::vector<NameSprite> g_nameSprites;
    void RequestSpriteAtlasRebuild();

    std::uint32_t EnsureNameSprite(const std::string& text)
    {
        if (text.empty())
            return 0;
        std::lock_guard<std::mutex> lock(g_nameSpriteMutex);
        for (const NameSprite& sprite : g_nameSprites)
        {
            if (sprite.text == text)
                return sprite.key;
        }
        if (g_nameSprites.size() >= MAX_NAME_SPRITES)
            return 0;
        NameSprite sprite{};
        sprite.key = SPRITE_KEY_NAME_BASE + static_cast<std::uint32_t>(g_nameSprites.size());
        sprite.text = text;
        g_nameSprites.push_back(sprite);
        RequestSpriteAtlasRebuild();
        return sprite.key;
    }

    std::vector<NameSprite> CopyNameSprites()
    {
        std::lock_guard<std::mutex> lock(g_nameSpriteMutex);
        return g_nameSprites;
    }

    // White text with a soft dark outline, on a transparent background.
    bool TryRenderTextSprite(const std::string& utf8, std::uint32_t key, SpriteImage& out)
    {
        const int wideLength = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
        if (wideLength <= 0 || wideLength > 64)
            return false;
        std::vector<wchar_t> wide(static_cast<std::size_t>(wideLength));
        if (MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), wide.data(), wideLength) != wideLength)
            return false;

        HDC screenDc = GetDC(nullptr);
        HDC dc = CreateCompatibleDC(screenDc);
        if (screenDc != nullptr)
            ReleaseDC(nullptr, screenDc);
        if (dc == nullptr)
            return false;

        bool success = false;
        constexpr int PAD = 3;
        const int requestedHeight = NameSpriteFontHeight();
        for (int fontHeight = requestedHeight; fontHeight >= 9 && !success; fontHeight -= 3)
        {
            HFONT font = CreateFontW(
                -fontHeight, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            if (font == nullptr)
                break;
            HGDIOBJ previousFont = SelectObject(dc, font);

            SIZE extent{};
            if (GetTextExtentPoint32W(dc, wide.data(), wideLength, &extent) && extent.cx > 0 && extent.cy > 0)
            {
                const std::uint32_t width = static_cast<std::uint32_t>(extent.cx) + PAD * 2;
                const std::uint32_t height = static_cast<std::uint32_t>(extent.cy) + PAD * 2;
                if (width <= NAME_SPRITE_MAX_WIDTH && height <= NAME_SPRITE_MAX_WIDTH)
                {
                    BITMAPINFO info{};
                    info.bmiHeader.biSize = sizeof(info.bmiHeader);
                    info.bmiHeader.biWidth = static_cast<LONG>(width);
                    info.bmiHeader.biHeight = -static_cast<LONG>(height);   // top-down
                    info.bmiHeader.biPlanes = 1;
                    info.bmiHeader.biBitCount = 32;
                    info.bmiHeader.biCompression = BI_RGB;
                    void* bits = nullptr;
                    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
                    if (bitmap != nullptr && bits != nullptr)
                    {
                        HGDIOBJ previousBitmap = SelectObject(dc, bitmap);
                        std::memset(bits, 0, static_cast<std::size_t>(width) * height * 4);
                        SetBkMode(dc, TRANSPARENT);
                        SetTextColor(dc, RGB(255, 255, 255));
                        if (TextOutW(dc, PAD, PAD, wide.data(), wideLength))
                        {
                            const auto* pixels = static_cast<const std::uint8_t*>(bits);
                            std::vector<std::uint8_t> coverage(static_cast<std::size_t>(width) * height, 0);
                            for (std::size_t i = 0; i < coverage.size(); ++i)
                            {
                                const std::uint8_t b = pixels[i * 4 + 0];
                                const std::uint8_t g = pixels[i * 4 + 1];
                                const std::uint8_t r = pixels[i * 4 + 2];
                                coverage[i] = MaxValue(r, MaxValue(g, b));
                            }

                            out.key = key;
                            out.width = width;
                            out.height = height;
                            out.rgba.assign(static_cast<std::size_t>(width) * height * 4, 0);
                            for (std::uint32_t y = 0; y < height; ++y)
                            {
                                for (std::uint32_t x = 0; x < width; ++x)
                                {
                                    const std::size_t index = static_cast<std::size_t>(y) * width + x;
                                    std::uint8_t outline = 0;
                                    for (int dy = -1; dy <= 1; ++dy)
                                    {
                                        for (int dx = -1; dx <= 1; ++dx)
                                        {
                                            const int sx = static_cast<int>(x) + dx;
                                            const int sy = static_cast<int>(y) + dy;
                                            if (sx < 0 || sy < 0 || sx >= static_cast<int>(width) || sy >= static_cast<int>(height))
                                                continue;
                                            outline = MaxValue(outline, coverage[static_cast<std::size_t>(sy) * width + static_cast<std::size_t>(sx)]);
                                        }
                                    }
                                    const float textAlpha = static_cast<float>(coverage[index]) / 255.0f;
                                    const float shadowAlpha = static_cast<float>(outline) / 255.0f * 0.85f;
                                    const float alpha = MaxValue(textAlpha, shadowAlpha);
                                    if (alpha <= 0.004f)
                                        continue;
                                    const float luminance = textAlpha / MaxValue(alpha, 0.0001f);
                                    const std::uint8_t value = static_cast<std::uint8_t>(ClampValue(luminance, 0.0f, 1.0f) * 255.0f + 0.5f);
                                    out.rgba[index * 4 + 0] = value;
                                    out.rgba[index * 4 + 1] = value;
                                    out.rgba[index * 4 + 2] = value;
                                    out.rgba[index * 4 + 3] = static_cast<std::uint8_t>(ClampValue(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
                                }
                            }
                            success = true;
                        }
                        SelectObject(dc, previousBitmap);
                    }
                    if (bitmap != nullptr)
                        DeleteObject(bitmap);
                }
            }

            SelectObject(dc, previousFont);
            DeleteObject(font);
        }

        DeleteDC(dc);
        return success;
    }
    // ---- END PLAYER NAME LABELS ----

    // A finished atlas, built on a worker thread: the packed image with its mip chain
    // and the sprite rectangles. The render thread only copies it into a staging
    // buffer and creates the pipeline (~10 ms for 4096 x 4096 instead of ~100 ms of
    // GDI text rendering, packing and mip generation on the frame).
    struct SpriteAtlasBuild
    {
        bool ok = false;
        std::uint32_t atlasSize = 0;
        std::uint32_t cell = 0;
        std::uint32_t levels = 1;
        std::vector<std::vector<std::uint8_t>> mips;
        std::vector<GpuSpriteRect> rects;
        int fontHeight = 0;
        std::uint64_t request = 0;
    };
    std::mutex g_spriteAtlasMutex;
    std::unique_ptr<SpriteAtlasBuild> g_pendingSpriteAtlas;     // guarded by g_spriteAtlasMutex
    std::atomic<bool> g_spriteAtlasWorkerBusy{ false };
    std::atomic<std::uint64_t> g_spriteAtlasRequest{ 1 };      // bumped whenever the atlas must change
    std::atomic<std::uint64_t> g_spriteAtlasStarted{ 0 };      // request the last worker was started for
    std::atomic<DWORD> g_spriteAtlasRequestTick{ 0 };
    std::atomic<int> g_spriteAtlasFailures{ 0 };

    void RequestSpriteAtlasRebuild()
    {
        g_spriteAtlasRequest.fetch_add(1);
        g_spriteAtlasRequestTick.store(GetTickCount());
    }

    // Everything CPU-side: renders the name labels, gathers the icons and their
    // silhouettes, packs the atlas and generates its mips. Safe on any thread.
    void BuildSpriteAtlasCpu(SpriteAtlasBuild& build)
    {
        std::vector<SpriteImage> images;
        images.push_back(BuildPlayerSprite());
        // Same lime fill and dark rim as the triangle.
        images.push_back(BuildDotSprite(SPRITE_KEY_PLAYER_DOT, 0.62f, 0.95f, 0.22f, 0.06f, 0.16f, 0.04f));
        images.push_back(BuildDotSprite(SPRITE_KEY_ALLY, 0.40f, 0.80f, 1.00f, 0.03f, 0.10f, 0.16f));
        images.push_back(BuildDotSprite(SPRITE_KEY_NPC, 1.00f, 0.85f, 0.20f, 0.20f, 0.14f, 0.02f));
        images.push_back(BuildWaypointRingSprite());
        build.fontHeight = NameSpriteFontHeight();
        for (const NameSprite& sprite : CopyNameSprites())
        {
            SpriteImage text{};
            if (TryRenderTextSprite(sprite.text, sprite.key, text))
                images.push_back(std::move(text));
        }
        EnsureMinimapIconsLoaded();
        {
            std::lock_guard<std::mutex> lock(g_minimapIconMutex);
            if (g_minimapIcons.loaded)
            {
                for (const MinimapIcon& icon : g_minimapIcons.icons)
                    images.push_back({ icon.key, icon.width, icon.height, icon.rgba });
                for (std::size_t index = 0; index < g_minimapIcons.icons.size() && index < 0xFFFFu; ++index)
                {
                    const MinimapIcon& icon = g_minimapIcons.icons[index];
                    images.push_back(BuildIconSilhouette(
                        SPRITE_KEY_SILHOUETTE_BASE + static_cast<std::uint32_t>(index), icon.rgba, icon.width, icon.height));
                }
            }
        }

        std::uint32_t maxSide = 1;
        for (SpriteImage& image : images)
        {
            BleedTransparentColor(image.rgba, image.width, image.height);
            maxSide = MaxValue(maxSide, MaxValue(image.width, image.height));
        }

        const std::uint32_t cell = maxSide + SPRITE_ATLAS_PADDING * 2;
        const std::uint32_t columns = static_cast<std::uint32_t>(std::ceil(std::sqrt(static_cast<double>(images.size()))));
        std::uint32_t atlasSize = 64;
        while (atlasSize < columns * cell && atlasSize < SPRITE_ATLAS_MAX_SIZE)
            atlasSize *= 2;
        if (columns * cell > atlasSize)
        {
            Log("[Minimap] GPU sprites unavailable (icon atlas too large)");
            return;
        }

        std::vector<std::uint8_t> atlas(static_cast<std::size_t>(atlasSize) * atlasSize * 4, 0);
        std::vector<GpuSpriteRect> rects;
        rects.reserve(images.size());
        for (std::size_t index = 0; index < images.size(); ++index)
        {
            const SpriteImage& image = images[index];
            const std::uint32_t cellX = static_cast<std::uint32_t>(index % columns) * cell;
            const std::uint32_t cellY = static_cast<std::uint32_t>(index / columns) * cell;
            const std::uint32_t originX = cellX + SPRITE_ATLAS_PADDING;
            const std::uint32_t originY = cellY + SPRITE_ATLAS_PADDING;
            // Copy with clamped edge extrusion into the padding (alpha 0 there).
            for (std::uint32_t y = 0; y < cell; ++y)
            {
                const int sy = static_cast<int>(y) - static_cast<int>(SPRITE_ATLAS_PADDING);
                const std::uint32_t cy = static_cast<std::uint32_t>(ClampValue(sy, 0, static_cast<int>(image.height) - 1));
                for (std::uint32_t x = 0; x < cell; ++x)
                {
                    const int sx = static_cast<int>(x) - static_cast<int>(SPRITE_ATLAS_PADDING);
                    const std::uint32_t cx = static_cast<std::uint32_t>(ClampValue(sx, 0, static_cast<int>(image.width) - 1));
                    const bool inside = sx >= 0 && sy >= 0 && sx < static_cast<int>(image.width) && sy < static_cast<int>(image.height);
                    const std::size_t src = (static_cast<std::size_t>(cy) * image.width + cx) * 4;
                    const std::size_t dst = (static_cast<std::size_t>(cellY + y) * atlasSize + (cellX + x)) * 4;
                    atlas[dst] = image.rgba[src];
                    atlas[dst + 1] = image.rgba[src + 1];
                    atlas[dst + 2] = image.rgba[src + 2];
                    atlas[dst + 3] = inside ? image.rgba[src + 3] : 0;
                }
            }

            GpuSpriteRect rect{};
            rect.key = image.key;
            rect.u0 = static_cast<float>(originX) / static_cast<float>(atlasSize);
            rect.v0 = static_cast<float>(originY) / static_cast<float>(atlasSize);
            rect.u1 = static_cast<float>(originX + image.width) / static_cast<float>(atlasSize);
            rect.v1 = static_cast<float>(originY + image.height) / static_cast<float>(atlasSize);
            rect.aspect = static_cast<float>(image.width) / static_cast<float>(MaxValue<std::uint32_t>(1, image.height));
            rects.push_back(rect);
        }

        // Mips stop while the padding still separates neighbouring sprites.
        std::uint32_t levels = 1;
        while (levels < SPRITE_ATLAS_MAX_LEVELS && (SPRITE_ATLAS_PADDING >> (levels - 1)) >= 2)
            ++levels;
        levels = MinValue(levels, ComputeMipLevelCount(atlasSize));

        build.mips.clear();
        build.mips.push_back(std::move(atlas));
        std::uint32_t dim = atlasSize;
        for (std::uint32_t level = 1; level < levels; ++level)
        {
            const std::uint32_t next = MaxValue<std::uint32_t>(1, dim >> 1);
            std::vector<std::uint8_t> smaller(static_cast<std::size_t>(next) * next * 4);
            DownsampleRgbaLevel(build.mips.back().data(), dim, smaller.data(), next);
            build.mips.push_back(std::move(smaller));
            dim = next;
        }

        build.atlasSize = atlasSize;
        build.cell = cell;
        build.levels = levels;
        build.rects = std::move(rects);
        build.ok = true;
    }

    void MaybeStartSpriteAtlasWorker()
    {
        const std::uint64_t request = g_spriteAtlasRequest.load();
        if (g_spriteAtlasStarted.load() == request)
            return;
        // Names tend to arrive in bursts (everyone joining at once): wait for a quiet
        // moment so one build covers them all.
        const DWORD requestTick = g_spriteAtlasRequestTick.load();
        if (requestTick != 0 && GetTickCount() - requestTick < 250)
            return;
        if (g_spriteAtlasWorkerBusy.exchange(true))
            return;
        g_spriteAtlasStarted.store(request);

        std::thread([request]()
        {
            BackgroundThreadScope scope;
            auto build = std::make_unique<SpriteAtlasBuild>();
            build->request = request;
            BuildSpriteAtlasCpu(*build);
            if (build->ok)
            {
                g_spriteAtlasFailures.store(0);
                std::lock_guard<std::mutex> lock(g_spriteAtlasMutex);
                g_pendingSpriteAtlas = std::move(build);
            }
            else
            {
                g_spriteAtlasFailures.fetch_add(1);
            }
            g_spriteAtlasWorkerBusy.store(false);
        }).detach();
    }

    bool TryCreateGpuSpriteResourcesLocked(VulkanMinimapRenderer& renderer)
    {
        GpuTexturePipeline& gp = renderer.spriteGpu;
        if (g_nameSpriteBuiltSize.load() != 0 && g_nameSpriteBuiltSize.load() != NameSpriteFontHeight())
        {
            g_nameSpriteBuiltSize.store(NameSpriteFontHeight());
            RequestSpriteAtlasRebuild();
        }

        std::unique_ptr<SpriteAtlasBuild> build;
        {
            std::lock_guard<std::mutex> lock(g_spriteAtlasMutex);
            build = std::move(g_pendingSpriteAtlas);
        }

        // A renderer rebuilt from scratch (resolution change, F10) has no sprite
        // pipeline and nothing pending: ask for a fresh build once. Repeated worker
        // failures stop the requests so a broken atlas cannot spin a thread forever.
        if (build == nullptr && !gp.ready && !gp.attempted && !g_spriteAtlasWorkerBusy.load() &&
            g_spriteAtlasStarted.load() == g_spriteAtlasRequest.load() && g_spriteAtlasFailures.load() < 3)
        {
            RequestSpriteAtlasRebuild();
        }

        if (CanStartGpuPipelineLocked(renderer))
            MaybeStartSpriteAtlasWorker();
        if (build == nullptr)
            return gp.ready;
        if (!CanStartGpuPipelineLocked(renderer))
        {
            // Renderer not up yet: keep the build for a later frame.
            std::lock_guard<std::mutex> lock(g_spriteAtlasMutex);
            if (g_pendingSpriteAtlas == nullptr)
                g_pendingSpriteAtlas = std::move(build);
            return gp.ready;
        }

        // Swap in the new atlas. The old one may still be referenced by submitted
        // command buffers, so it is parked and destroyed a few frames later instead of
        // stalling the device (vkDeviceWaitIdle races the game's own submitting
        // threads and can lose the device).
        if (gp.ready)
        {
            RetiredGpuPipeline retired{};
            retired.gp = gp;
            renderer.retiredPipelines.push_back(std::move(retired));
            gp = GpuTexturePipeline{};
        }
        else
        {
            DestroyGpuTexturePipelineLocked(renderer, gp);
        }
        renderer.spriteRects.clear();
        gp.attempted = true;

        if (!TryCreateGpuTexturePipelineLocked(renderer, gp, MINIMAP_SPRITE_FRAGMENT_SHADER, build->mips.front().data(), build->atlasSize, build->levels, "sprites", false, &build->mips))
        {
            Log("[Minimap] using CPU icon fallback");
            return false;
        }

        g_nameSpriteBuiltSize.store(build->fontHeight);
        renderer.spriteRects = std::move(build->rects);
        std::ostringstream oss;
        oss << "[Minimap] GPU sprite renderer ready"
            << " | sprites=" << renderer.spriteRects.size()
            << " | atlas=" << build->atlasSize << "x" << build->atlasSize
            << " | cell=" << build->cell
            << " | mip_levels=" << gp.mipLevels
            << " | built_off_thread=yes";
        Log(oss.str());
        return true;
    }

    const GpuSpriteRect* FindGpuSprite(const VulkanMinimapRenderer& renderer, std::uint32_t key)
    {
        for (const GpuSpriteRect& rect : renderer.spriteRects)
        {
            if (rect.key == key)
                return &rect;
        }
        return nullptr;
    }

    bool IsGpuPipelineDrawable(const VulkanMinimapRenderer& renderer, const GpuTexturePipeline& gp)
    {
        return gp.ready && !gp.uploadPending && gp.pipeline != 0 && gp.pipelineLayout != 0 && gp.descriptorSet != 0 &&
            renderer.width != 0 && renderer.height != 0;
    }

    // Draws sprite `key` centered at (centerX, centerY), `sizePx` tall. Unrotated
    // sprites are clipped exactly to the square window (clipCx, clipCy, clipHalf).
    // Rotation is in radians, clockwise on screen. Returns false only when the sprite
    // cannot be drawn on the GPU (caller falls back to the CPU path).
    bool TryDrawSpriteGpu(
        VulkanMinimapRenderer& renderer,
        void* commandBuffer,
        std::uint32_t key,
        float centerX,
        float centerY,
        float sizePx,
        float rotation,
        float alpha,
        int clipCx,
        int clipCy,
        int clipHalf,
        float tintRed = 1.0f,
        float tintGreen = 1.0f,
        float tintBlue = 1.0f);

    bool TryDrawSpriteGpu(
        VulkanMinimapRenderer& renderer,
        void* commandBuffer,
        std::uint32_t key,
        float centerX,
        float centerY,
        float sizePx,
        float rotation,
        float alpha,
        int clipCx,
        int clipCy,
        int clipHalf,
        float tintRed,
        float tintGreen,
        float tintBlue)
    {
        if (!IsGpuPipelineDrawable(renderer, renderer.spriteGpu))
            return false;
        const GpuSpriteRect* rect = FindGpuSprite(renderer, key);
        if (rect == nullptr)
            return false;

        const float halfH = sizePx * 0.5f;
        const float halfW = halfH * rect->aspect;
        float left = centerX - halfW;
        float right = centerX + halfW;
        float top = centerY - halfH;
        float bottom = centerY + halfH;
        float u0 = rect->u0;
        float v0 = rect->v0;
        float u1 = rect->u1;
        float v1 = rect->v1;

        const bool rotated = std::fabs(rotation) > 0.0001f;
        if (!rotated)
        {
            const float clipLeft = static_cast<float>(clipCx - clipHalf);
            const float clipRight = static_cast<float>(clipCx + clipHalf + 1);
            const float clipTop = static_cast<float>(clipCy - clipHalf);
            const float clipBottom = static_cast<float>(clipCy + clipHalf + 1);
            if (right <= clipLeft || left >= clipRight || bottom <= clipTop || top >= clipBottom)
                return true;
            const float du = (u1 - u0) / (right - left);
            const float dv = (v1 - v0) / (bottom - top);
            if (left < clipLeft) { u0 += (clipLeft - left) * du; left = clipLeft; }
            if (right > clipRight) { u1 -= (right - clipRight) * du; right = clipRight; }
            if (top < clipTop) { v0 += (clipTop - top) * dv; top = clipTop; }
            if (bottom > clipBottom) { v1 -= (bottom - clipBottom) * dv; bottom = clipBottom; }
        }

        const float width = static_cast<float>(renderer.width);
        const float height = static_cast<float>(renderer.height);
        const float push[16] = {
            (left / width) * 2.0f - 1.0f,
            (top / height) * 2.0f - 1.0f,
            (right / width) * 2.0f - 1.0f,
            (bottom / height) * 2.0f - 1.0f,
            std::cos(rotation),
            std::sin(rotation) * GPU_SPRITE_ROTATION_SIGN,
            height / width,
            width / height,
            u0,
            v0,
            u1,
            v1,
            tintRed,
            tintGreen,
            tintBlue,
            alpha
        };

        const GpuTexturePipeline& gp = renderer.spriteGpu;
        void* descriptorSet = reinterpret_cast<void*>(gp.descriptorSet);
        void* pipelineLayout = reinterpret_cast<void*>(gp.pipelineLayout);
        // Every icon, label and dot draws from the same pipeline and descriptor set;
        // rebinding them per sprite is pure driver overhead (~100 sprites a frame).
        if (renderer.boundPipeline != gp.pipeline)
        {
            renderer.fns.cmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, reinterpret_cast<void*>(gp.pipeline));
            renderer.boundPipeline = gp.pipeline;
        }
        if (renderer.boundDescriptorSet != gp.descriptorSet)
        {
            renderer.fns.cmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
            renderer.boundDescriptorSet = gp.descriptorSet;
        }
        renderer.fns.cmdPushConstants(commandBuffer, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, MINIMAP_MAP_PUSH_BYTES, push);
        renderer.fns.cmdDraw(commandBuffer, 6, 1, 0, 0);
        return true;
    }

    // ---- shared upload / teardown ----

    // Must be recorded before the render pass begins.
    void RecordGpuTextureUploadIfNeeded(VulkanMinimapRenderer& renderer, GpuTexturePipeline& gp, void* commandBuffer, std::uint32_t imageIndex)
    {
        if (!gp.ready || !gp.uploadPending)
            return;

        void* image = reinterpret_cast<void*>(gp.image);
        void* buffer = reinterpret_cast<void*>(gp.stagingBuffer);
        if (image == nullptr || buffer == nullptr || gp.uploadRegions.empty())
            return;

        // A re-upload must not overwrite the image while an earlier frame is still
        // sampling it, so it waits on fragment-shader reads; the first upload has
        // nothing to wait for.
        const bool reupload = gp.uploadedOnce;
        VkImageMemoryBarrier toTransfer{};
        toTransfer.srcAccessMask = reupload ? VK_ACCESS_SHADER_READ_BIT : 0;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toTransfer.oldLayout = reupload ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.image = image;
        toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toTransfer.subresourceRange.levelCount = gp.mipLevels;
        toTransfer.subresourceRange.layerCount = 1;
        renderer.fns.cmdPipelineBarrier(
            commandBuffer,
            reupload ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,
            0,
            nullptr,
            0,
            nullptr,
            1,
            &toTransfer);

        renderer.fns.cmdCopyBufferToImage(
            commandBuffer,
            buffer,
            image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            static_cast<std::uint32_t>(gp.uploadRegions.size()),
            gp.uploadRegions.data());

        VkImageMemoryBarrier toShader{};
        toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toShader.image = image;
        toShader.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toShader.subresourceRange.levelCount = gp.mipLevels;
        toShader.subresourceRange.layerCount = 1;
        renderer.fns.cmdPipelineBarrier(
            commandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0,
            0,
            nullptr,
            0,
            nullptr,
            1,
            &toShader);

        gp.uploadPending = false;
        gp.uploadedOnce = true;
        // The staging buffer (hundreds of MB for an 8192 map) is freed the next time
        // this image index is recorded: by then its fence has been waited on, so the
        // upload submit is guaranteed to have finished.
        gp.uploadImageIndex = imageIndex;
        gp.stagingReleasePending = true;
    }

    void RecordGpuMapUploadIfNeeded(VulkanMinimapRenderer& renderer, void* commandBuffer, std::uint32_t imageIndex)
    {
        RecordGpuTextureUploadIfNeeded(renderer, renderer.mapGpu, commandBuffer, imageIndex);
        RecordGpuTextureUploadIfNeeded(renderer, renderer.spriteGpu, commandBuffer, imageIndex);
        RecordGpuTextureUploadIfNeeded(renderer, renderer.fogGpu, commandBuffer, imageIndex);
    }

    void ReleaseGpuMapStagingIfUploadedLocked(VulkanMinimapRenderer& renderer, std::uint32_t imageIndex)
    {
        for (GpuTexturePipeline* gp : { &renderer.mapGpu, &renderer.spriteGpu, &renderer.fogGpu })
        {
            if (gp->stagingReleasePending && gp->uploadImageIndex == imageIndex)
            {
                if (gp->persistentStaging)
                {
                    // The upload recorded with this image has completed: the buffer may
                    // be refilled again.
                    gp->stagingReleasePending = false;
                    gp->stagingBusy = false;
                    continue;
                }
                ReleaseGpuTextureStagingLocked(renderer, *gp);
                if (gp == &renderer.mapGpu && g_minimapMapGpuEnabled.load())
                    ReleaseRealMapCpuCopy();
            }
        }
    }

    // Called once per recorded frame, after this image's fence has been waited on. A
    // retired pipeline goes once every swapchain image has been re-recorded since it
    // was parked: no submitted command buffer can still reference it by then.
    void DestroyRetiredGpuPipelinesIfSafeLocked(VulkanMinimapRenderer& renderer, std::uint32_t imageIndex)
    {
        if (renderer.retiredPipelines.empty())
            return;

        const std::uint32_t imageCount = static_cast<std::uint32_t>(renderer.commandBuffers.size());
        const std::uint32_t allImages = imageCount >= 32 ? 0xFFFFFFFFu : ((1u << imageCount) - 1u);
        for (std::size_t i = 0; i < renderer.retiredPipelines.size();)
        {
            RetiredGpuPipeline& retired = renderer.retiredPipelines[i];
            if (imageIndex < 32)
                retired.seenImageMask |= (1u << imageIndex);
            ++retired.records;
            const bool cycled = (retired.seenImageMask & allImages) == allImages && retired.records > imageCount;
            const bool stale = retired.records > 240;   // safety net if an image index never comes back
            if (cycled || stale)
            {
                DestroyGpuTexturePipelineLocked(renderer, retired.gp);
                renderer.retiredPipelines.erase(renderer.retiredPipelines.begin() + static_cast<std::ptrdiff_t>(i));
            }
            else
            {
                ++i;
            }
        }
    }

    void DestroyGpuMapResourcesLocked(VulkanMinimapRenderer& renderer)
    {
        DestroyGpuTexturePipelineLocked(renderer, renderer.mapGpu);
        DestroyGpuTexturePipelineLocked(renderer, renderer.spriteGpu);
        DestroyGpuTexturePipelineLocked(renderer, renderer.fogGpu);
        for (RetiredGpuPipeline& retired : renderer.retiredPipelines)
            DestroyGpuTexturePipelineLocked(renderer, retired.gp);
        renderer.retiredPipelines.clear();
        renderer.spriteRects.clear();
    }

    bool TryDrawRealMapGpu(VulkanMinimapRenderer& renderer, void* commandBuffer, int cx, int cy, int radius, float centerX, float centerZ, float unitsPerPixel, float headingRadians)
    {
        const GpuTexturePipeline& gp = renderer.mapGpu;
        if (!g_minimapMapGpuEnabled.load() || !IsGpuPipelineDrawable(renderer, gp))
            return false;

        // Same footprint as the CPU rasterizer: pixel offsets -r..r around (cx, cy).
        const int innerRadius = MaxValue(8, radius);
        const float width = static_cast<float>(renderer.width);
        const float height = static_cast<float>(renderer.height);
        const float left = static_cast<float>(cx - innerRadius);
        const float top = static_cast<float>(cy - innerRadius);
        const float right = static_cast<float>(cx + innerRadius + 1);
        const float bottom = static_cast<float>(cy + innerRadius + 1);
        const float radiusPixels = static_cast<float>(innerRadius) + 0.5f;

        const float push[16] = {
            // frame vertex shader: quad rect in NDC + rotation (identity) + aspect terms
            (left / width) * 2.0f - 1.0f,
            (top / height) * 2.0f - 1.0f,
            (right / width) * 2.0f - 1.0f,
            (bottom / height) * 2.0f - 1.0f,
            1.0f,
            0.0f,
            height / width,
            width / height,
            // map fragment shader: view
            centerX / REAL_MAP_WORLD_SIZE,
            1.0f - (centerZ / REAL_MAP_WORLD_SIZE),
            (radiusPixels * unitsPerPixel) / REAL_MAP_WORLD_SIZE,
            radiusPixels,
            // map fragment shader: params
            std::cos(headingRadians),
            std::sin(headingRadians),
            MINIMAP_GPU_MAP_VIGNETTE,
            1.0f        // shroud overlay strength (alpha channel carries the shroud distance)
        };

        void* descriptorSet = reinterpret_cast<void*>(gp.descriptorSet);
        void* pipelineLayout = reinterpret_cast<void*>(gp.pipelineLayout);
        renderer.fns.cmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, reinterpret_cast<void*>(gp.pipeline));
        renderer.boundPipeline = gp.pipeline;
        renderer.fns.cmdBindDescriptorSets(
            commandBuffer,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            pipelineLayout,
            0,
            1,
            &descriptorSet,
            0,
            nullptr);
        renderer.boundDescriptorSet = gp.descriptorSet;
        renderer.fns.cmdPushConstants(
            commandBuffer,
            pipelineLayout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0,
            MINIMAP_MAP_PUSH_BYTES,
            push);
        renderer.fns.cmdDraw(commandBuffer, 6, 1, 0, 0);
        return true;
    }
    // ---- fog of war ----
    constexpr const char* MINIMAP_FOG_FRAGMENT_SHADER = "embervale_minimap_fog.frag.spv";
    constexpr std::uint32_t FOG_TEXTURE_MAX_LEVELS = 3;

    // Resamples the live grid into a square RGBA image in map-uv space (u = x / W,
    // v = 1 - z / W with W = REAL_MAP_WORLD_SIZE), so the fog shader can reuse the map
    // shader's push constants unchanged. With the shipped 10240-unit world and 8-unit
    // cells this is a straight copy (1280 x 1280). The value goes into every channel.
    bool BuildFogTextureFromGrid(std::vector<std::uint8_t>& rgba, std::uint32_t& outSize, std::uint64_t& outVersion)
    {
        std::lock_guard<std::mutex> lock(g_fogGridMutex);
        const FogOfWarGrid& grid = g_fogGrid;
        if (!grid.valid || grid.width == 0 || grid.height == 0 || grid.cells.size() < static_cast<std::size_t>(grid.width) * grid.height)
            return false;

        std::uint32_t size = MaxValue(grid.width, grid.height);
        size = ClampValue<std::uint32_t>(size + (size & 1u), 64, 4096);
        rgba.resize(static_cast<std::size_t>(size) * size * 4);

        const bool direct = grid.width == size && grid.height == size &&
            std::fabs(grid.sizeX - REAL_MAP_WORLD_SIZE) < 1.0f && std::fabs(grid.sizeZ - REAL_MAP_WORLD_SIZE) < 1.0f;
        const float cellX = grid.sizeX / static_cast<float>(grid.width);
        const float cellZ = grid.sizeZ / static_cast<float>(grid.height);
        for (std::uint32_t j = 0; j < size; ++j)
        {
            std::uint32_t row = j;
            if (!direct)
            {
                const float z = (1.0f - (static_cast<float>(j) + 0.5f) / static_cast<float>(size)) * REAL_MAP_WORLD_SIZE;
                row = static_cast<std::uint32_t>(ClampValue(static_cast<int>((grid.sizeZ - z) / cellZ), 0, static_cast<int>(grid.height) - 1));
            }
            const std::uint8_t* src = grid.cells.data() + static_cast<std::size_t>(row) * grid.width;
            std::uint8_t* dst = rgba.data() + static_cast<std::size_t>(j) * size * 4;
            for (std::uint32_t i = 0; i < size; ++i)
            {
                std::uint32_t col = i;
                if (!direct)
                {
                    const float x = (static_cast<float>(i) + 0.5f) / static_cast<float>(size) * REAL_MAP_WORLD_SIZE;
                    col = static_cast<std::uint32_t>(ClampValue(static_cast<int>(x / cellX), 0, static_cast<int>(grid.width) - 1));
                }
                const std::uint8_t value = src[col];
                dst[i * 4 + 0] = value;
                dst[i * 4 + 1] = value;
                dst[i * 4 + 2] = value;
                dst[i * 4 + 3] = value;
            }
        }
        outSize = size;
        outVersion = grid.version;
        return true;
    }

    bool TryCreateGpuFogResourcesLocked(VulkanMinimapRenderer& renderer)
    {
        GpuTexturePipeline& gp = renderer.fogGpu;
        if (gp.ready || gp.attempted || g_minimapFogStrength.load() <= 0)
            return gp.ready;
        if (!CanStartGpuPipelineLocked(renderer))
            return false;

        std::vector<std::uint8_t> rgba;
        std::uint32_t size = 0;
        std::uint64_t version = 0;
        if (!BuildFogTextureFromGrid(rgba, size, version))
            return false;   // no grid yet (world still loading); try again next frame

        gp.attempted = true;
        if (!TryCreateGpuTexturePipelineLocked(renderer, gp, MINIMAP_FOG_FRAGMENT_SHADER, rgba.data(), size, FOG_TEXTURE_MAX_LEVELS, "fog", true))
            return false;
        gp.contentVersion = version;

        std::ostringstream oss;
        oss << "[Minimap] GPU fog of war ready"
            << " | texture=" << size << "x" << size
            << " | mip_levels=" << gp.mipLevels;
        Log(oss.str());
        return true;
    }

    // Refills the fog staging buffer when the grid changed and no upload is in flight.
    void UpdateGpuFogIfNeededLocked(VulkanMinimapRenderer& renderer)
    {
        GpuTexturePipeline& gp = renderer.fogGpu;
        if (!gp.ready || !gp.persistentStaging || gp.stagingMapped == nullptr || gp.stagingBusy || gp.uploadPending)
            return;

        std::uint64_t version = 0;
        {
            std::lock_guard<std::mutex> lock(g_fogGridMutex);
            version = g_fogGrid.version;
        }
        if (version == gp.contentVersion)
            return;

        static std::vector<std::uint8_t> rgba;   // render thread, under g_rendererMutex
        std::uint32_t size = 0;
        if (!BuildFogTextureFromGrid(rgba, size, version) || size != gp.size)
        {
            // The grid is gone (world left) or has another size: retire this texture and
            // let it be rebuilt when the next grid arrives.
            RetiredGpuPipeline retired{};
            retired.gp = gp;
            renderer.retiredPipelines.push_back(std::move(retired));
            gp = GpuTexturePipeline{};
            return;
        }

        WriteMipChainToStaging(gp.uploadRegions, gp.stagingMapped, rgba.data(), size);
        gp.contentVersion = version;
        gp.uploadPending = true;
        gp.stagingBusy = true;
    }

    // Darkens the undiscovered part of the map. Same quad and push constants as the
    // map draw; params.w carries the strength.
    bool TryDrawFogGpu(VulkanMinimapRenderer& renderer, void* commandBuffer, int cx, int cy, int radius, float centerX, float centerZ, float unitsPerPixel, float headingRadians)
    {
        const GpuTexturePipeline& gp = renderer.fogGpu;
        const int strengthPercent = g_minimapFogStrength.load();
        if (strengthPercent <= 0 || !IsGpuPipelineDrawable(renderer, gp))
            return false;

        const int innerRadius = MaxValue(8, radius);
        const float width = static_cast<float>(renderer.width);
        const float height = static_cast<float>(renderer.height);
        const float left = static_cast<float>(cx - innerRadius);
        const float top = static_cast<float>(cy - innerRadius);
        const float right = static_cast<float>(cx + innerRadius + 1);
        const float bottom = static_cast<float>(cy + innerRadius + 1);
        const float radiusPixels = static_cast<float>(innerRadius) + 0.5f;

        const float push[16] = {
            (left / width) * 2.0f - 1.0f,
            (top / height) * 2.0f - 1.0f,
            (right / width) * 2.0f - 1.0f,
            (bottom / height) * 2.0f - 1.0f,
            1.0f,
            0.0f,
            height / width,
            width / height,
            centerX / REAL_MAP_WORLD_SIZE,
            1.0f - (centerZ / REAL_MAP_WORLD_SIZE),
            (radiusPixels * unitsPerPixel) / REAL_MAP_WORLD_SIZE,
            radiusPixels,
            std::cos(headingRadians),
            std::sin(headingRadians),
            0.0f,
            static_cast<float>(strengthPercent) / 100.0f
        };

        void* descriptorSet = reinterpret_cast<void*>(gp.descriptorSet);
        void* pipelineLayout = reinterpret_cast<void*>(gp.pipelineLayout);
        renderer.fns.cmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, reinterpret_cast<void*>(gp.pipeline));
        renderer.boundPipeline = gp.pipeline;
        renderer.fns.cmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
        renderer.boundDescriptorSet = gp.descriptorSet;
        renderer.fns.cmdPushConstants(commandBuffer, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, MINIMAP_MAP_PUSH_BYTES, push);
        renderer.fns.cmdDraw(commandBuffer, 6, 1, 0, 0);
        return true;
    }
    // ---- END GPU MAP RENDERER ----

    void DestroyVulkanMinimapRendererLocked()
    {
        void* device = reinterpret_cast<void*>(g_renderer.device);
        const VulkanRendererFns fns = g_renderer.fns;

        // Everything we are about to destroy is referenced only by our own command
        // buffers, and each of those has a fence. Waiting on the fences is enough and,
        // unlike vkDeviceWaitIdle, does not race the game's own submitting threads (a
        // known way to lose the device). The device-wide wait stays as the fallback for
        // a fence that never signals.
        if (device != nullptr)
        {
            bool allSignaled = true;
            if (fns.waitForFences != nullptr)
            {
                for (uintptr_t fenceHandle : g_renderer.commandFences)
                {
                    if (fenceHandle == 0)
                        continue;
                    void* fence = reinterpret_cast<void*>(fenceHandle);
                    if (fns.waitForFences(device, 1, &fence, 1, 500000000ull) != VK_SUCCESS)
                        allSignaled = false;
                }
            }
            else
            {
                allSignaled = false;
            }
            if (!allSignaled && fns.deviceWaitIdle != nullptr)
                fns.deviceWaitIdle(device);
        }

        if (device != nullptr)
        {
            DestroyGpuMapResourcesLocked(g_renderer);

            if (g_renderer.framePipeline != 0 && fns.destroyPipeline != nullptr)
                fns.destroyPipeline(device, reinterpret_cast<void*>(g_renderer.framePipeline), nullptr);
            if (g_renderer.framePipelineLayout != 0 && fns.destroyPipelineLayout != nullptr)
                fns.destroyPipelineLayout(device, reinterpret_cast<void*>(g_renderer.framePipelineLayout), nullptr);
            if (g_renderer.frameDescriptorPool != 0 && fns.destroyDescriptorPool != nullptr)
                fns.destroyDescriptorPool(device, reinterpret_cast<void*>(g_renderer.frameDescriptorPool), nullptr);
            if (g_renderer.frameDescriptorSetLayout != 0 && fns.destroyDescriptorSetLayout != nullptr)
                fns.destroyDescriptorSetLayout(device, reinterpret_cast<void*>(g_renderer.frameDescriptorSetLayout), nullptr);
            if (g_renderer.frameSampler != 0 && fns.destroySampler != nullptr)
                fns.destroySampler(device, reinterpret_cast<void*>(g_renderer.frameSampler), nullptr);
            if (g_renderer.frameTextureImageView != 0 && fns.destroyImageView != nullptr)
                fns.destroyImageView(device, reinterpret_cast<void*>(g_renderer.frameTextureImageView), nullptr);
            if (g_renderer.frameTextureImage != 0 && fns.destroyImage != nullptr)
                fns.destroyImage(device, reinterpret_cast<void*>(g_renderer.frameTextureImage), nullptr);
            if (g_renderer.frameTextureMemory != 0 && fns.freeMemory != nullptr)
                fns.freeMemory(device, reinterpret_cast<void*>(g_renderer.frameTextureMemory), nullptr);
            if (g_renderer.frameStagingBuffer != 0 && fns.destroyBuffer != nullptr)
                fns.destroyBuffer(device, reinterpret_cast<void*>(g_renderer.frameStagingBuffer), nullptr);
            if (g_renderer.frameStagingMemory != 0 && fns.freeMemory != nullptr)
                fns.freeMemory(device, reinterpret_cast<void*>(g_renderer.frameStagingMemory), nullptr);

            if (fns.destroySemaphore != nullptr)
            {
                for (uintptr_t semaphore : g_renderer.renderCompleteSemaphores)
                {
                    if (semaphore != 0)
                        fns.destroySemaphore(device, reinterpret_cast<void*>(semaphore), nullptr);
                }
            }

            if (fns.destroyFence != nullptr)
            {
                for (uintptr_t fence : g_renderer.commandFences)
                {
                    if (fence != 0)
                        fns.destroyFence(device, reinterpret_cast<void*>(fence), nullptr);
                }
            }

            if (fns.destroyFramebuffer != nullptr)
            {
                for (uintptr_t framebuffer : g_renderer.framebuffers)
                {
                    if (framebuffer != 0)
                        fns.destroyFramebuffer(device, reinterpret_cast<void*>(framebuffer), nullptr);
                }
            }

            if (g_renderer.renderPass != 0 && fns.destroyRenderPass != nullptr)
                fns.destroyRenderPass(device, reinterpret_cast<void*>(g_renderer.renderPass), nullptr);

            if (fns.destroyImageView != nullptr)
            {
                for (uintptr_t imageView : g_renderer.imageViews)
                {
                    if (imageView != 0)
                        fns.destroyImageView(device, reinterpret_cast<void*>(imageView), nullptr);
                }
            }

            if (g_renderer.commandPool != 0 && fns.destroyCommandPool != nullptr)
                fns.destroyCommandPool(device, reinterpret_cast<void*>(g_renderer.commandPool), nullptr);
        }

        g_renderer = VulkanMinimapRenderer{};
    }

    bool FetchRendererSwapchainImages(VulkanMinimapRenderer& renderer, const SwapchainRuntimeInfo& snapshot)
    {
        if (!snapshot.images.empty())
        {
            renderer.images = snapshot.images;
            return true;
        }

        std::uint32_t count = snapshot.imageCount;
        void* device = reinterpret_cast<void*>(renderer.device);
        void* swapchain = reinterpret_cast<void*>(renderer.swapchain);
        if (count == 0)
        {
            if (renderer.fns.getSwapchainImages(device, swapchain, &count, nullptr) != VK_SUCCESS)
                return false;
        }

        if (count == 0 || count > 16)
            return false;

        renderer.images.assign(count, 0);
        return renderer.fns.getSwapchainImages(device, swapchain, &count, renderer.images.data()) == VK_SUCCESS;
    }

    bool CreateRendererCommandPoolLocked(std::uint32_t preferredFamily)
    {
        std::vector<std::uint32_t> candidates;
        candidates.push_back(preferredFamily);
        for (std::uint32_t family = 0; family < 8; ++family)
        {
            if (std::find(candidates.begin(), candidates.end(), family) == candidates.end())
                candidates.push_back(family);
        }

        void* device = reinterpret_cast<void*>(g_renderer.device);
        for (std::uint32_t family : candidates)
        {
            VkCommandPoolCreateInfo commandPoolInfo{};
            commandPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            commandPoolInfo.queueFamilyIndex = family;

            void* commandPool = nullptr;
            if (g_renderer.fns.createCommandPool(device, &commandPoolInfo, nullptr, &commandPool) == VK_SUCCESS && commandPool != nullptr)
            {
                g_renderer.commandPool = reinterpret_cast<uintptr_t>(commandPool);
                g_renderer.queueFamilyIndex = family;
                return true;
            }
        }

        return false;
    }

    bool BuildVulkanMinimapRendererLocked(const SwapchainRuntimeInfo& snapshot)
    {
        const std::uint32_t fallbackWidth = static_cast<std::uint32_t>(MaxValue(1, GetSystemMetrics(SM_CXSCREEN)));
        const std::uint32_t fallbackHeight = static_cast<std::uint32_t>(MaxValue(1, GetSystemMetrics(SM_CYSCREEN)));
        const std::uint32_t width = snapshot.width != 0 ? snapshot.width : fallbackWidth;
        const std::uint32_t height = snapshot.height != 0 ? snapshot.height : fallbackHeight;
        const std::uint32_t format = snapshot.format != 0 ? snapshot.format : 37;

        // Compare the image-handle set too: when the game recreates its swapchain the
        // handle value can be reused, so matching only handle+extent would keep stale
        // framebuffers/image views and fault the driver on submit. A changed image set
        // forces a rebuild.
        // When the snapshot carries image handles, a mismatch means the swapchain was
        // recreated (possibly at the same handle value) and our objects are stale. When
        // it carries none, fall back to handle+extent identity so we don't thrash.
        const bool imagesConsistent = snapshot.images.empty() || g_renderer.images == snapshot.images;
        if (g_renderer.ready &&
            g_renderer.device == snapshot.device &&
            g_renderer.swapchain == snapshot.handle &&
            g_renderer.format == format &&
            g_renderer.width == width &&
            g_renderer.height == height &&
            imagesConsistent)
        {
            return true;
        }

        DestroyVulkanMinimapRendererLocked();

        g_renderer.device = snapshot.device;
        g_renderer.swapchain = snapshot.handle;
        g_renderer.format = format;
        g_renderer.width = width;
        g_renderer.height = height;

        void* device = reinterpret_cast<void*>(g_renderer.device);
        if (device == nullptr || g_renderer.swapchain == 0 || !TryLoadVulkanRendererFns(g_renderer.fns, device))
        {
            LogRendererThrottled("[Minimap] Vulkan minimap renderer not ready: missing device functions");
            DestroyVulkanMinimapRendererLocked();
            return false;
        }

        if (!FetchRendererSwapchainImages(g_renderer, snapshot))
        {
            LogRendererThrottled("[Minimap] Vulkan minimap renderer not ready: swapchain images unavailable");
            DestroyVulkanMinimapRendererLocked();
            return false;
        }

        if (g_renderer.images.empty())
        {
            DestroyVulkanMinimapRendererLocked();
            return false;
        }

        if (!CreateRendererCommandPoolLocked(g_lastVulkanQueueFamilyIndex.load()))
        {
            LogRendererThrottled("[Minimap] Vulkan minimap renderer not ready: command pool creation failed");
            DestroyVulkanMinimapRendererLocked();
            return false;
        }

        VkAttachmentDescription colorAttachment{};
        colorAttachment.format = g_renderer.format;
        VkAttachmentReference colorReference{};
        VkSubpassDescription subpass{};
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorReference;
        VkRenderPassCreateInfo renderPassInfo{};
        renderPassInfo.attachmentCount = 1;
        renderPassInfo.pAttachments = &colorAttachment;
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;

        void* renderPass = nullptr;
        if (g_renderer.fns.createRenderPass(device, &renderPassInfo, nullptr, &renderPass) != VK_SUCCESS || renderPass == nullptr)
        {
            LogRendererThrottled("[Minimap] Vulkan minimap renderer not ready: render pass creation failed");
            DestroyVulkanMinimapRendererLocked();
            return false;
        }
        g_renderer.renderPass = reinterpret_cast<uintptr_t>(renderPass);

        const bool texturedFrameReady = TryCreateTexturedFrameResourcesLocked(g_renderer);
        if (!texturedFrameReady)
            LogRendererThrottled("[Minimap] textured minimap frame unavailable; using clear-rect frame fallback");

        g_renderer.imageViews.assign(g_renderer.images.size(), 0);
        g_renderer.framebuffers.assign(g_renderer.images.size(), 0);
        for (std::size_t index = 0; index < g_renderer.images.size(); ++index)
        {
            VkImageViewCreateInfo imageViewInfo{};
            imageViewInfo.image = reinterpret_cast<void*>(g_renderer.images[index]);
            imageViewInfo.format = g_renderer.format;
            imageViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            imageViewInfo.subresourceRange.levelCount = 1;
            imageViewInfo.subresourceRange.layerCount = 1;

            void* imageView = nullptr;
            if (g_renderer.fns.createImageView(device, &imageViewInfo, nullptr, &imageView) != VK_SUCCESS || imageView == nullptr)
            {
                LogRendererThrottled("[Minimap] Vulkan minimap renderer not ready: image view creation failed");
                DestroyVulkanMinimapRendererLocked();
                return false;
            }
            g_renderer.imageViews[index] = reinterpret_cast<uintptr_t>(imageView);

            void* attachment = imageView;
            VkFramebufferCreateInfo framebufferInfo{};
            framebufferInfo.renderPass = renderPass;
            framebufferInfo.attachmentCount = 1;
            framebufferInfo.pAttachments = &attachment;
            framebufferInfo.width = g_renderer.width;
            framebufferInfo.height = g_renderer.height;
            framebufferInfo.layers = 1;

            void* framebuffer = nullptr;
            if (g_renderer.fns.createFramebuffer(device, &framebufferInfo, nullptr, &framebuffer) != VK_SUCCESS || framebuffer == nullptr)
            {
                LogRendererThrottled("[Minimap] Vulkan minimap renderer not ready: framebuffer creation failed");
                DestroyVulkanMinimapRendererLocked();
                return false;
            }
            g_renderer.framebuffers[index] = reinterpret_cast<uintptr_t>(framebuffer);
        }

        g_renderer.commandBuffers.assign(g_renderer.images.size(), 0);
        VkCommandBufferAllocateInfo allocateInfo{};
        allocateInfo.commandPool = reinterpret_cast<void*>(g_renderer.commandPool);
        allocateInfo.commandBufferCount = static_cast<std::uint32_t>(g_renderer.commandBuffers.size());
        if (g_renderer.fns.allocateCommandBuffers(device, &allocateInfo, reinterpret_cast<void**>(g_renderer.commandBuffers.data())) != VK_SUCCESS)
        {
            LogRendererThrottled("[Minimap] Vulkan minimap renderer not ready: command buffer allocation failed");
            DestroyVulkanMinimapRendererLocked();
            return false;
        }

        g_renderer.renderCompleteSemaphores.assign(g_renderer.images.size(), 0);
        VkSemaphoreCreateInfo semaphoreInfo{};
        for (std::size_t index = 0; index < g_renderer.renderCompleteSemaphores.size(); ++index)
        {
            void* semaphore = nullptr;
            if (g_renderer.fns.createSemaphore(device, &semaphoreInfo, nullptr, &semaphore) != VK_SUCCESS || semaphore == nullptr)
            {
                LogRendererThrottled("[Minimap] Vulkan minimap renderer not ready: semaphore creation failed");
                DestroyVulkanMinimapRendererLocked();
                return false;
            }
            g_renderer.renderCompleteSemaphores[index] = reinterpret_cast<uintptr_t>(semaphore);
        }

        // Per-image fences (created signaled): resetting a command buffer that the GPU
        // may still be executing is undefined behavior and can take down the device;
        // each re-record now waits for the previous submit of that image to finish.
        g_renderer.commandFences.assign(g_renderer.images.size(), 0);
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for (std::size_t index = 0; index < g_renderer.commandFences.size(); ++index)
        {
            void* fence = nullptr;
            if (g_renderer.fns.createFence(device, &fenceInfo, nullptr, &fence) != VK_SUCCESS || fence == nullptr)
            {
                LogRendererThrottled("[Minimap] Vulkan minimap renderer not ready: fence creation failed");
                DestroyVulkanMinimapRendererLocked();
                return false;
            }
            g_renderer.commandFences[index] = reinterpret_cast<uintptr_t>(fence);
        }

        g_renderer.ready = true;

        std::ostringstream oss;
        oss << "[Minimap] Vulkan minimap renderer ready"
            << " | swapchain=" << Hex(g_renderer.swapchain)
            << " | extent=" << g_renderer.width << "x" << g_renderer.height
            << " | format=" << g_renderer.format
            << " | images=" << g_renderer.images.size()
            << " | queue_family=" << g_renderer.queueFamilyIndex
            << " | draw=clear-attachment-widget";
        Log(oss.str());
        return true;
    }

    bool AppendClippedClearRect(VulkanMinimapRenderer& renderer, std::vector<VkClearRect>& rects, int x, int y, int width, int height)
    {
        if (width <= 0 || height <= 0)
            return false;

        const int maxWidth = static_cast<int>(renderer.width);
        const int maxHeight = static_cast<int>(renderer.height);
        if (x < 0)
        {
            width += x;
            x = 0;
        }
        if (y < 0)
        {
            height += y;
            y = 0;
        }
        if (x >= maxWidth || y >= maxHeight)
            return false;

        width = MinValue(width, maxWidth - x);
        height = MinValue(height, maxHeight - y);
        if (width <= 0 || height <= 0)
            return false;

        VkClearRect rect{};
        rect.rect.offset.x = x;
        rect.rect.offset.y = y;
        rect.rect.extent.width = static_cast<std::uint32_t>(width);
        rect.rect.extent.height = static_cast<std::uint32_t>(height);
        rects.push_back(rect);
        return true;
    }

    void CmdClearRects(VulkanMinimapRenderer& renderer, void* commandBuffer, float red, float green, float blue, float alpha, const std::vector<VkClearRect>& rects)
    {
        if (rects.empty())
            return;

        VkClearAttachment attachment{};
        attachment.clearValue.color.float32[0] = red;
        attachment.clearValue.color.float32[1] = green;
        attachment.clearValue.color.float32[2] = blue;
        attachment.clearValue.color.float32[3] = alpha;

        renderer.fns.cmdClearAttachments(commandBuffer, 1, &attachment, static_cast<std::uint32_t>(rects.size()), rects.data());
    }

    void CmdClearRect(VulkanMinimapRenderer& renderer, void* commandBuffer, float red, float green, float blue, float alpha, int x, int y, int width, int height)
    {
        if (width <= 0 || height <= 0)
            return;

        const int maxWidth = static_cast<int>(renderer.width);
        const int maxHeight = static_cast<int>(renderer.height);
        if (x < 0)
        {
            width += x;
            x = 0;
        }
        if (y < 0)
        {
            height += y;
            y = 0;
        }
        if (x >= maxWidth || y >= maxHeight)
            return;

        width = MinValue(width, maxWidth - x);
        height = MinValue(height, maxHeight - y);
        if (width <= 0 || height <= 0)
            return;

        VkClearAttachment attachment{};
        attachment.clearValue.color.float32[0] = red;
        attachment.clearValue.color.float32[1] = green;
        attachment.clearValue.color.float32[2] = blue;
        attachment.clearValue.color.float32[3] = alpha;

        VkClearRect rect{};
        rect.rect.offset.x = x;
        rect.rect.offset.y = y;
        rect.rect.extent.width = static_cast<std::uint32_t>(width);
        rect.rect.extent.height = static_cast<std::uint32_t>(height);
        renderer.fns.cmdClearAttachments(commandBuffer, 1, &attachment, 1, &rect);
    }

    // The minimap window is a square: "within radius r of the center" means within the
    // square of half-size r (Chebyshev distance), for the map, markers and the arrow.
    int MinimapWindowDistance(int dx, int dy)
    {
        return MaxValue(std::abs(dx), std::abs(dy));
    }

    bool IsInsideMinimapWindow(int dx, int dy, int halfSize)
    {
        return MinimapWindowDistance(dx, dy) <= halfSize;
    }

    void CmdClearCircle(VulkanMinimapRenderer& renderer, void* commandBuffer, float red, float green, float blue, float alpha, int cx, int cy, int radius, int stripHeight)
    {
        if (radius <= 0)
            return;

        stripHeight = MaxValue(1, stripHeight);
        const int radiusSquared = radius * radius;
        std::vector<VkClearRect> rects;
        rects.reserve(static_cast<std::size_t>((radius * 2) / stripHeight + 3));
        for (int dy = -radius; dy <= radius; dy += stripHeight)
        {
            const int sampleY = MinValue(radius, dy + (stripHeight / 2));
            const int halfWidth = static_cast<int>(std::sqrt(static_cast<float>(MaxValue(0, radiusSquared - sampleY * sampleY))));
            AppendClippedClearRect(renderer, rects, cx - halfWidth, cy + dy, halfWidth * 2 + 1, stripHeight);
        }
        CmdClearRects(renderer, commandBuffer, red, green, blue, alpha, rects);
    }

    void CmdClearDotInCircle(VulkanMinimapRenderer& renderer, void* commandBuffer, int cx, int cy, int radius, int x, int y, int size, float red, float green, float blue)
    {
        const int dx = x - cx;
        const int dy = y - cy;
        if (!IsInsideMinimapWindow(dx, dy, radius - size))
            return;

        CmdClearRect(renderer, commandBuffer, red, green, blue, 1.0f, x - size / 2, y - size / 2, size, size);
    }

    void CmdClearPolyline(VulkanMinimapRenderer& renderer, void* commandBuffer, int cx, int cy, int radius, int x0, int y0, int x1, int y1, int size, float red, float green, float blue)
    {
        const int steps = MaxValue(1, MaxValue(std::abs(x1 - x0), std::abs(y1 - y0)) / MaxValue(1, size / 2));
        for (int step = 0; step <= steps; ++step)
        {
            const float t = static_cast<float>(step) / static_cast<float>(steps);
            const int x = static_cast<int>(static_cast<float>(x0) + static_cast<float>(x1 - x0) * t);
            const int y = static_cast<int>(static_cast<float>(y0) + static_cast<float>(y1 - y0) * t);
            CmdClearDotInCircle(renderer, commandBuffer, cx, cy, radius, x, y, size, red, green, blue);
        }
    }

    void CmdClearMarker(VulkanMinimapRenderer& renderer, void* commandBuffer, int cx, int cy, int radius, int x, int y, float red, float green, float blue)
    {
        CmdClearDotInCircle(renderer, commandBuffer, cx, cy, radius, x, y - 5, 6, red, green, blue);
        CmdClearDotInCircle(renderer, commandBuffer, cx, cy, radius, x - 5, y, 6, red, green, blue);
        CmdClearDotInCircle(renderer, commandBuffer, cx, cy, radius, x, y, 8, 1.0f, 0.94f, 0.72f);
        CmdClearDotInCircle(renderer, commandBuffer, cx, cy, radius, x + 5, y, 6, red, green, blue);
        CmdClearDotInCircle(renderer, commandBuffer, cx, cy, radius, x, y + 5, 6, red, green, blue);
    }

    void CmdClearSolidDiamond(VulkanMinimapRenderer& renderer, void* commandBuffer, int x, int y, int size, float red, float green, float blue, float alpha = 1.0f)
    {
        if (size <= 0)
            return;

        std::vector<VkClearRect> rects;
        rects.reserve(static_cast<std::size_t>(size + 2));
        for (int dy = -size; dy <= size; dy += 2)
        {
            const int halfWidth = size - std::abs(dy);
            AppendClippedClearRect(renderer, rects, x - halfWidth, y + dy, halfWidth * 2 + 1, 2);
        }
        CmdClearRects(renderer, commandBuffer, red, green, blue, alpha, rects);
    }

    void CmdClearDiamond(VulkanMinimapRenderer& renderer, void* commandBuffer, int cx, int cy, int radius, int x, int y, int size, float red, float green, float blue)
    {
        CmdClearSolidDiamond(renderer, commandBuffer, x, y, size, red, green, blue);
        CmdClearDotInCircle(renderer, commandBuffer, cx, cy, radius, x, y, MaxValue(3, size / 2), 1.0f, 0.95f, 0.72f);
    }

    void CmdClearOutlinedDiamond(VulkanMinimapRenderer& renderer, void* commandBuffer, int cx, int cy, int radius, int x, int y, int size, float red, float green, float blue)
    {
        CmdClearSolidDiamond(renderer, commandBuffer, x, y, size + 2, 0.035f, 0.028f, 0.020f);
        CmdClearSolidDiamond(renderer, commandBuffer, x, y, size, red, green, blue);
        CmdClearDotInCircle(renderer, commandBuffer, cx, cy, radius, x, y, MaxValue(3, size / 2), 1.0f, 0.95f, 0.76f);
    }

    void CmdClearVerticalTriangle(VulkanMinimapRenderer& renderer, void* commandBuffer, int centerX, int tipY, int baseY, int baseHalfWidth, float red, float green, float blue)
    {
        const int height = MaxValue(1, baseY - tipY);
        std::vector<VkClearRect> rects;
        rects.reserve(static_cast<std::size_t>(height / 2 + 3));
        for (int y = tipY; y <= baseY; y += 2)
        {
            const float t = static_cast<float>(y - tipY) / static_cast<float>(height);
            const int halfWidth = MaxValue(1, static_cast<int>(t * static_cast<float>(baseHalfWidth)));
            AppendClippedClearRect(renderer, rects, centerX - halfWidth, y, halfWidth * 2 + 1, 2);
        }
        CmdClearRects(renderer, commandBuffer, red, green, blue, 1.0f, rects);
    }

    void CmdClearSmallCircle(VulkanMinimapRenderer& renderer, void* commandBuffer, int cx, int cy, int radius, int x, int y, int drawRadius, float red, float green, float blue, float alpha = 1.0f)
    {
        if (drawRadius <= 0)
            return;

        const int dx = x - cx;
        const int dy = y - cy;
        const int allowedRadius = MaxValue(1, radius - drawRadius);
        if (!IsInsideMinimapWindow(dx, dy, allowedRadius))
            return;

        const int radiusSq = drawRadius * drawRadius;
        std::vector<VkClearRect> rects;
        rects.reserve(static_cast<std::size_t>(drawRadius + 2));
        for (int offsetY = -drawRadius; offsetY <= drawRadius; offsetY += 2)
        {
            const int halfWidth = static_cast<int>(std::sqrt(static_cast<float>(MaxValue(0, radiusSq - offsetY * offsetY))));
            AppendClippedClearRect(renderer, rects, x - halfWidth, y + offsetY, halfWidth * 2 + 1, 2);
        }
        CmdClearRects(renderer, commandBuffer, red, green, blue, alpha, rects);
    }

    bool TryDrawMinimapRasterIcon(VulkanMinimapRenderer& renderer, void* commandBuffer, int cx, int cy, int radius, int x, int y, std::uint32_t kind, bool clipped);
    bool HasLoadedMinimapIconAtlas();

    void CmdClearPoiBackplate(VulkanMinimapRenderer& renderer, void* commandBuffer, int cx, int cy, int radius, int x, int y, int size, float red, float green, float blue)
    {
        // Big-map style: bright icon on a light parchment pill, no black shadow ring.
        CmdClearSmallCircle(renderer, commandBuffer, cx, cy, radius, x, y, size + 4, 0.99f, 0.97f, 0.90f, 0.92f);
        CmdClearSmallCircle(renderer, commandBuffer, cx, cy, radius, x, y, size + 2, red, green, blue, 0.95f);
        CmdClearSmallCircle(renderer, commandBuffer, cx, cy, radius, x, y, size, 0.99f, 0.97f, 0.90f, 0.95f);
    }

    void CmdClearQuestIcon(VulkanMinimapRenderer& renderer, void* commandBuffer, int cx, int cy, int radius, int x, int y, int size, float red, float green, float blue)
    {
        CmdClearPoiBackplate(renderer, commandBuffer, cx, cy, radius, x, y, size, red, green, blue);
        CmdClearSolidDiamond(renderer, commandBuffer, x, y, MaxValue(4, size - 1), red, green, blue);
        CmdClearRect(renderer, commandBuffer, 1.0f, 0.93f, 0.72f, 1.0f, x - 1, y - size + 2, 3, MaxValue(4, size));
        CmdClearSmallCircle(renderer, commandBuffer, cx, cy, radius, x, y + size - 2, 2, 1.0f, 0.93f, 0.72f);
    }

    // Player-placed waypoint: red flag on a light pole, mirroring the in-game marker.
    void CmdClearFlagIcon(VulkanMinimapRenderer& renderer, void* commandBuffer, int cx, int cy, int radius, int x, int y, int size)
    {
        CmdClearPoiBackplate(renderer, commandBuffer, cx, cy, radius, x, y, size, 0.92f, 0.18f, 0.14f);
        const int poleHeight = size * 2 + 2;
        const int poleTop = y - size - 2;
        CmdClearRect(renderer, commandBuffer, 0.97f, 0.94f, 0.86f, 1.0f, x - 1, poleTop, 2, poleHeight);
        const int flagWidth = MaxValue(5, size + 1);
        const int flagHeight = MaxValue(4, size - 1);
        CmdClearRect(renderer, commandBuffer, 0.96f, 0.16f, 0.12f, 1.0f, x + 1, poleTop, flagWidth, flagHeight);
        CmdClearRect(renderer, commandBuffer, 0.72f, 0.08f, 0.06f, 1.0f, x + 1, poleTop + flagHeight - 1, flagWidth, 1);
    }

    void CmdClearHomeIcon(VulkanMinimapRenderer& renderer, void* commandBuffer, int cx, int cy, int radius, int x, int y, int size, float red, float green, float blue)
    {
        CmdClearPoiBackplate(renderer, commandBuffer, cx, cy, radius, x, y, size, red, green, blue);
        CmdClearVerticalTriangle(renderer, commandBuffer, x, y - size - 3, y, size, red, green, blue);
        CmdClearRect(renderer, commandBuffer, red, green, blue, 1.0f, x - size + 2, y - 1, MaxValue(3, size * 2 - 3), size);
        CmdClearRect(renderer, commandBuffer, 0.035f, 0.030f, 0.026f, 1.0f, x - 1, y + 1, 3, size - 1);
    }

    void CmdClearFlameIcon(VulkanMinimapRenderer& renderer, void* commandBuffer, int cx, int cy, int radius, int x, int y, int size)
    {
        CmdClearPoiBackplate(renderer, commandBuffer, cx, cy, radius, x, y, size, 1.0f, 0.74f, 0.10f);
        CmdClearSolidDiamond(renderer, commandBuffer, x, y + 1, MaxValue(4, size - 1), 1.0f, 0.66f, 0.08f);
        CmdClearVerticalTriangle(renderer, commandBuffer, x, y - size - 4, y + size - 1, MaxValue(4, size - 2), 1.0f, 0.80f, 0.16f);
        CmdClearVerticalTriangle(renderer, commandBuffer, x, y - size + 1, y + 2, MaxValue(2, size / 2), 1.0f, 0.96f, 0.50f);
    }

    void CmdClearSpireIcon(VulkanMinimapRenderer& renderer, void* commandBuffer, int cx, int cy, int radius, int x, int y, int size, float red, float green, float blue)
    {
        CmdClearPoiBackplate(renderer, commandBuffer, cx, cy, radius, x, y, size, red, green, blue);
        CmdClearVerticalTriangle(renderer, commandBuffer, x, y - size - 5, y + size, MaxValue(4, size - 1), red, green, blue);
        CmdClearRect(renderer, commandBuffer, 1.0f, 0.94f, 0.78f, 1.0f, x - 1, y - size + 1, 3, size * 2 - 1);
        CmdClearRect(renderer, commandBuffer, red, green, blue, 1.0f, x - size, y + size - 1, size * 2 + 1, 3);
    }

    void CmdClearMineIcon(VulkanMinimapRenderer& renderer, void* commandBuffer, int cx, int cy, int radius, int x, int y, int size)
    {
        CmdClearPoiBackplate(renderer, commandBuffer, cx, cy, radius, x, y, size, 0.98f, 0.48f, 0.62f);
        CmdClearPolyline(renderer, commandBuffer, cx, cy, radius, x - size, y + size, x + size, y - size, 3, 1.0f, 0.92f, 0.78f);
        CmdClearPolyline(renderer, commandBuffer, cx, cy, radius, x - size, y - 1, x + size, y - size - 2, 3, 0.98f, 0.56f, 0.68f);
    }

    void CmdClearCampIcon(VulkanMinimapRenderer& renderer, void* commandBuffer, int cx, int cy, int radius, int x, int y, int size)
    {
        CmdClearPoiBackplate(renderer, commandBuffer, cx, cy, radius, x, y, size, 0.90f, 0.66f, 0.36f);
        CmdClearVerticalTriangle(renderer, commandBuffer, x, y - size - 1, y + size, size, 0.98f, 0.92f, 0.78f);
        CmdClearVerticalTriangle(renderer, commandBuffer, x, y, y + size, MaxValue(2, size / 2), 0.055f, 0.045f, 0.034f);
    }

    void CmdClearDangerIcon(VulkanMinimapRenderer& renderer, void* commandBuffer, int cx, int cy, int radius, int x, int y, int size, float red, float green, float blue)
    {
        CmdClearPoiBackplate(renderer, commandBuffer, cx, cy, radius, x, y, size, red, green, blue);
        CmdClearPolyline(renderer, commandBuffer, cx, cy, radius, x - size, y - size, x + size, y + size, 4, red, green, blue);
        CmdClearPolyline(renderer, commandBuffer, cx, cy, radius, x - size, y + size, x + size, y - size, 4, red, green, blue);
        CmdClearSmallCircle(renderer, commandBuffer, cx, cy, radius, x, y, 2, 1.0f, 0.92f, 0.76f);
    }

    void CmdClearWaterIcon(VulkanMinimapRenderer& renderer, void* commandBuffer, int cx, int cy, int radius, int x, int y, int size)
    {
        CmdClearPoiBackplate(renderer, commandBuffer, cx, cy, radius, x, y, size, 0.22f, 0.78f, 1.0f);
        CmdClearSmallCircle(renderer, commandBuffer, cx, cy, radius, x, y + 1, MaxValue(3, size - 2), 0.22f, 0.78f, 1.0f);
        CmdClearVerticalTriangle(renderer, commandBuffer, x, y - size - 3, y + 2, MaxValue(3, size - 3), 0.70f, 0.96f, 1.0f);
    }

    void CmdClearGenericPoiIcon(VulkanMinimapRenderer& renderer, void* commandBuffer, int cx, int cy, int radius, int x, int y, int size, float red, float green, float blue)
    {
        CmdClearPoiBackplate(renderer, commandBuffer, cx, cy, radius, x, y, size, red, green, blue);
        CmdClearOutlinedDiamond(renderer, commandBuffer, cx, cy, radius, x, y, MaxValue(3, size - 2), red, green, blue);
    }

    void CmdClearPoiIcon(VulkanMinimapRenderer& renderer, void* commandBuffer, int cx, int cy, int radius, int x, int y, std::uint32_t kind, bool clipped)
    {
        if (TryDrawMinimapRasterIcon(renderer, commandBuffer, cx, cy, radius, x, y, kind, clipped))
            return;

        const int size = clipped ? 5 : 7;
        if (kind == MAP_MARKER_QUEST_IMPORTANT_KIND)
        {
            CmdClearQuestIcon(renderer, commandBuffer, cx, cy, radius, x, y, clipped ? 8 : 12, 1.0f, 0.74f, 0.10f);
            return;
        }

        if (kind == MAP_MARKER_QUEST_KIND)
        {
            CmdClearQuestIcon(renderer, commandBuffer, cx, cy, radius, x, y, clipped ? 6 : 9, 1.0f, 0.86f, 0.18f);
            return;
        }

        if (kind > 0xFFFFu && HasLoadedMinimapIconAtlas())
            return;

        switch (kind)
        {
        case 10:
            CmdClearQuestIcon(renderer, commandBuffer, cx, cy, radius, x, y, clipped ? 7 : 10, 1.0f, 0.72f, 0.10f);
            break;
        case 11:
            CmdClearFlagIcon(renderer, commandBuffer, cx, cy, radius, x, y, clipped ? 7 : 10);
            break;
        case 12:
            // NPC / crafter: small light-blue figure dot on a light pill.
            CmdClearSmallCircle(renderer, commandBuffer, cx, cy, radius, x, y, 5, 0.99f, 0.97f, 0.90f, 0.88f);
            CmdClearSmallCircle(renderer, commandBuffer, cx, cy, radius, x, y, 3, 0.30f, 0.66f, 0.96f, 1.0f);
            CmdClearSmallCircle(renderer, commandBuffer, cx, cy, radius, x, y - 3, 2, 0.44f, 0.76f, 1.0f, 1.0f);
            break;
        case 13:
            // Multiplayer ping: green diamond, like the big map.
            CmdClearPoiBackplate(renderer, commandBuffer, cx, cy, radius, x, y, size, 0.18f, 0.78f, 0.30f);
            CmdClearSolidDiamond(renderer, commandBuffer, x, y, MaxValue(4, size - 1), 0.20f, 0.82f, 0.32f);
            CmdClearRect(renderer, commandBuffer, 0.06f, 0.28f, 0.10f, 1.0f, x - 1, y - size / 2, 3, MaxValue(3, size - 2));
            break;
        case 2:
        case 40:
            CmdClearQuestIcon(renderer, commandBuffer, cx, cy, radius, x, y, size, 1.0f, 0.24f, 0.18f);
            break;
        case 3:
        case 30:
        case 42:
        case 47:
            CmdClearHomeIcon(renderer, commandBuffer, cx, cy, radius, x, y, size, 0.30f, 0.88f, 1.0f);
            break;
        case 31:
        case 32:
        case 44:
        case 53:
            CmdClearFlameIcon(renderer, commandBuffer, cx, cy, radius, x, y, size);
            break;
        case 33:
        case 45:
        case 54:
            CmdClearWaterIcon(renderer, commandBuffer, cx, cy, radius, x, y, size);
            break;
        case 34:
        case 43:
        case 49:
        case 50:
            CmdClearSpireIcon(renderer, commandBuffer, cx, cy, radius, x, y, size, 0.72f, 0.36f, 0.98f);
            break;
        case 41:
            CmdClearDangerIcon(renderer, commandBuffer, cx, cy, radius, x, y, size, 0.98f, 0.42f, 0.16f);
            break;
        case 46:
            CmdClearMineIcon(renderer, commandBuffer, cx, cy, radius, x, y, size);
            break;
        case 48:
            CmdClearSpireIcon(renderer, commandBuffer, cx, cy, radius, x, y, size, 0.95f, 0.62f, 0.18f);
            break;
        case 51:
            CmdClearCampIcon(renderer, commandBuffer, cx, cy, radius, x, y, size);
            break;
        case 52:
            CmdClearDangerIcon(renderer, commandBuffer, cx, cy, radius, x, y, size, 1.0f, 0.22f, 0.30f);
            break;
        case 55:
            CmdClearGenericPoiIcon(renderer, commandBuffer, cx, cy, radius, x, y, size, 0.84f, 0.78f, 0.58f);
            break;
        default:
            CmdClearGenericPoiIcon(renderer, commandBuffer, cx, cy, radius, x, y, size, 0.78f, 0.60f, 1.0f);
            break;
        }
    }


    void CmdClearFreeLine(VulkanMinimapRenderer& renderer, void* commandBuffer, int x0, int y0, int x1, int y1, int size, float red, float green, float blue);

    // Fallback arrow for north-up mode: a rotated outline (clockwise radians, 0 = up).
    // CPU fallback for the player marker: lime-green triangle (dark outline) pointing
    // along `rotation` (clockwise from north, y-down screen space).
    void CmdClearPlayerTriangle(VulkanMinimapRenderer& renderer, void* commandBuffer, int arrowX, int arrowY, float size, float rotation)
    {
        const float c = std::cos(rotation);
        const float s = std::sin(rotation);
        const auto fill = [&](float scale, float red, float green, float blue)
        {
            const float h = size * 0.5f * scale;
            float xs[3];
            float ys[3];
            const float local[3][2] = { { 0.0f, -h }, { -h * 0.62f, h * 0.84f }, { h * 0.62f, h * 0.84f } };
            for (int i = 0; i < 3; ++i)
            {
                xs[i] = static_cast<float>(arrowX) + c * local[i][0] - s * local[i][1];
                ys[i] = static_cast<float>(arrowY) + s * local[i][0] + c * local[i][1];
            }
            const int minY = static_cast<int>(std::floor(MinValue(ys[0], MinValue(ys[1], ys[2]))));
            const int maxY = static_cast<int>(std::ceil(MaxValue(ys[0], MaxValue(ys[1], ys[2]))));
            std::vector<VkClearRect> rects;
            for (int y = minY; y <= maxY; ++y)
            {
                const float py = static_cast<float>(y) + 0.5f;
                float lo = 1e9f;
                float hi = -1e9f;
                for (int i = 0; i < 3; ++i)
                {
                    const int j = (i + 1) % 3;
                    const float y0 = ys[i];
                    const float y1 = ys[j];
                    if ((py < MinValue(y0, y1)) || (py > MaxValue(y0, y1)) || std::fabs(y1 - y0) < 0.0001f)
                        continue;
                    const float t = (py - y0) / (y1 - y0);
                    const float x = xs[i] + (xs[j] - xs[i]) * t;
                    lo = MinValue(lo, x);
                    hi = MaxValue(hi, x);
                }
                if (hi < lo)
                    continue;
                const int x0 = static_cast<int>(std::lround(lo));
                const int x1 = static_cast<int>(std::lround(hi));
                AppendClippedClearRect(renderer, rects, x0, y, MaxValue(1, x1 - x0), 1);
            }
            CmdClearRects(renderer, commandBuffer, red, green, blue, 1.0f, rects);
        };
        fill(1.25f, 0.06f, 0.16f, 0.04f);
        fill(1.0f, 0.62f, 0.95f, 0.22f);
    }

    std::vector<CapturedWaypoint> CopyWaypoints()
    {
        std::lock_guard<std::mutex> lock(g_waypointMutex);
        return g_waypoints;
    }

    std::vector<CapturedNearbyMarker> CopyNearbyMarkers()
    {
        std::lock_guard<std::mutex> lock(g_nearbyMarkerMutex);
        return g_nearbyMarkers;
    }

    std::vector<CapturedMapMarkerVisibility> CopyVisibleMapMarkers()
    {
        const DWORD now = GetTickCount();
        std::lock_guard<std::mutex> lock(g_visibleMapMarkerMutex);

        std::vector<CapturedMapMarkerVisibility> result;
        result.reserve(g_visibleMapMarkers.size());
        for (const CapturedMapMarkerVisibility& marker : g_visibleMapMarkers)
        {
            if (marker.visibility == 0 || now - marker.lastUpdateTick > VISIBLE_MAP_MARKER_STALE_MS)
                continue;

            result.push_back(marker);
        }

        return result;
    }

    std::string JoinPath(const std::string& base, const std::string& name)
    {
        if (base.empty())
            return name;

        const char last = base.back();
        if (last == '\\' || last == '/')
            return base + name;

        return base + "\\" + name;
    }

    std::string GetExecutableDirectory()
    {
        char path[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameA(nullptr, path, static_cast<DWORD>(sizeof(path)));
        if (length == 0 || length >= sizeof(path))
            return {};

        char* slash = std::strrchr(path, '\\');
        if (slash != nullptr)
            *slash = '\0';

        return path;
    }

    bool TryResolveRgbaMapDimensions(std::streamoff byteSize, int& outTextureSize)
    {
        if (byteSize <= 0 || (byteSize % 4) != 0)
            return false;

        const std::uint64_t pixelCount = static_cast<std::uint64_t>(byteSize / 4);
        const int textureSize = static_cast<int>(std::sqrt(static_cast<double>(pixelCount)) + 0.5);
        if (textureSize < REAL_MAP_MIN_TEXTURE_SIZE || textureSize > REAL_MAP_MAX_TEXTURE_SIZE)
            return false;

        if (static_cast<std::uint64_t>(textureSize) * static_cast<std::uint64_t>(textureSize) != pixelCount)
            return false;

        outTextureSize = textureSize;
        return true;
    }

    void AddRealMapCandidates(std::vector<std::string>& candidates, const std::string& directory)
    {
        const char* names[] = {
            // Any square size 512..8192 (size is inferred from the byte count).
            "embervale_realmap_hd.rgba",
            "embervale_realmap_1280.rgba",
            "embervale_realmap_1024.rgba",
            "embervale_realmap_768.rgba",
            "embervale_realmap_512.rgba"
        };

        for (const char* name : names)
            candidates.push_back(JoinPath(directory, name));
    }

    bool TryLoadRealMapFromPath(const std::string& path, RealMapTexture& map)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file)
            return false;

        const std::streamoff size = file.tellg();
        int textureSize = 0;
        if (!TryResolveRgbaMapDimensions(size, textureSize))
            return false;

        file.seekg(0, std::ios::beg);
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(size));
        if (!file.read(reinterpret_cast<char*>(pixels.data()), pixels.size()))
            return false;

        map.loaded = true;
        map.width = textureSize;
        map.height = textureSize;
        map.path = path;
        map.rgba = std::move(pixels);
        return true;
    }

    // The HD map ships as a square grid of equally sized square tiles,
    // embervale_realmap_hd_<row>_<col>.rgba (row 0 = north, col 0 = west), so that
    // no single asset file is hundreds of MB (GitHub rejects files over 100 MB).
    constexpr const char* REAL_MAP_TILE_PREFIX = "embervale_realmap_hd_";
    constexpr int REAL_MAP_MAX_TILES_PER_SIDE = 16;

    std::string RealMapTilePath(const std::string& directory, int row, int column)
    {
        return JoinPath(directory, std::string(REAL_MAP_TILE_PREFIX) + std::to_string(row) + "_" + std::to_string(column) + ".rgba");
    }

    bool TryLoadTiledRealMap(const std::string& directory, RealMapTexture& map)
    {
        std::streamoff tileBytes = 0;
        {
            std::ifstream first(RealMapTilePath(directory, 0, 0), std::ios::binary | std::ios::ate);
            if (!first)
                return false;
            tileBytes = first.tellg();
        }

        if (tileBytes <= 0 || (tileBytes % 4) != 0)
            return false;
        const std::uint64_t tilePixels = static_cast<std::uint64_t>(tileBytes / 4);
        const int tileSize = static_cast<int>(std::sqrt(static_cast<double>(tilePixels)) + 0.5);
        if (tileSize < 64 || static_cast<std::uint64_t>(tileSize) * static_cast<std::uint64_t>(tileSize) != tilePixels)
            return false;

        int tilesPerSide = 0;
        while (tilesPerSide < REAL_MAP_MAX_TILES_PER_SIDE && std::ifstream(RealMapTilePath(directory, 0, tilesPerSide), std::ios::binary).good())
            ++tilesPerSide;

        const int mapSize = tilesPerSide * tileSize;
        if (mapSize < REAL_MAP_MIN_TEXTURE_SIZE || mapSize > REAL_MAP_MAX_TEXTURE_SIZE)
            return false;

        const std::size_t tileStride = static_cast<std::size_t>(tileSize) * 4;
        const std::size_t mapStride = static_cast<std::size_t>(mapSize) * 4;
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(mapSize) * mapStride);
        std::vector<std::uint8_t> tile(static_cast<std::size_t>(tileBytes));
        for (int row = 0; row < tilesPerSide; ++row)
        {
            for (int column = 0; column < tilesPerSide; ++column)
            {
                const std::string path = RealMapTilePath(directory, row, column);
                std::ifstream file(path, std::ios::binary | std::ios::ate);
                if (!file || file.tellg() != tileBytes)
                {
                    Log("[Minimap] HD map tile missing or wrong size: " + path);
                    return false;
                }

                file.seekg(0, std::ios::beg);
                if (!file.read(reinterpret_cast<char*>(tile.data()), static_cast<std::streamsize>(tile.size())))
                    return false;

                for (int y = 0; y < tileSize; ++y)
                {
                    const std::size_t dst = (static_cast<std::size_t>(row) * static_cast<std::size_t>(tileSize) + static_cast<std::size_t>(y)) * mapStride +
                        static_cast<std::size_t>(column) * tileStride;
                    std::memcpy(pixels.data() + dst, tile.data() + static_cast<std::size_t>(y) * tileStride, tileStride);
                }
            }
        }

        map.loaded = true;
        map.width = mapSize;
        map.height = mapSize;
        map.path = RealMapTilePath(directory, 0, 0) + " (" + std::to_string(tilesPerSide) + "x" + std::to_string(tilesPerSide) + " tiles)";
        map.rgba = std::move(pixels);
        return true;
    }

    // Shroud (FogZone) overlay. embervale_shroud_sdf.r8 is a square uint8 grid (row 0 =
    // north) holding the signed distance to the shroud border: 128 on the border,
    // 128 + d * 127 / 48 with d in world units, positive outside the shroud. It is
    // resampled into the map texture's alpha channel, which the GPU map shader and the
    // CPU fallback read; 255 (the default alpha) means "no shroud".
    constexpr const char* SHROUD_SDF_FILE = "embervale_shroud_sdf.r8";
    constexpr float SHROUD_SDF_RANGE = 48.0f;

    bool TryApplyShroudSdf(const std::vector<std::string>& directories, RealMapTexture& map)
    {
        if (!map.loaded || map.width <= 0 || map.width != map.height ||
            map.rgba.size() != static_cast<std::size_t>(map.width) * static_cast<std::size_t>(map.height) * 4)
        {
            return false;
        }

        for (const std::string& directory : directories)
        {
            const std::string path = JoinPath(directory, SHROUD_SDF_FILE);
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file)
                continue;

            const std::streamoff bytes = file.tellg();
            const int size = static_cast<int>(std::sqrt(static_cast<double>(bytes)) + 0.5);
            if (size < 64 || size > 16384 || static_cast<std::streamoff>(size) * size != bytes)
            {
                Log("[Minimap] shroud overlay file has an unexpected size: " + path);
                continue;
            }

            std::vector<std::uint8_t> sdf(static_cast<std::size_t>(bytes));
            file.seekg(0, std::ios::beg);
            if (!file.read(reinterpret_cast<char*>(sdf.data()), static_cast<std::streamsize>(sdf.size())))
                continue;

            // Bilinear resample, texel centers aligned.
            const int mapSize = map.width;
            const float scale = static_cast<float>(size) / static_cast<float>(mapSize);
            std::vector<int> x0s(static_cast<std::size_t>(mapSize));
            std::vector<int> x1s(static_cast<std::size_t>(mapSize));
            std::vector<float> txs(static_cast<std::size_t>(mapSize));
            for (int x = 0; x < mapSize; ++x)
            {
                const float fx = (static_cast<float>(x) + 0.5f) * scale - 0.5f;
                const int ix = static_cast<int>(std::floor(fx));
                x0s[static_cast<std::size_t>(x)] = ClampValue(ix, 0, size - 1);
                x1s[static_cast<std::size_t>(x)] = ClampValue(ix + 1, 0, size - 1);
                txs[static_cast<std::size_t>(x)] = fx - static_cast<float>(ix);
            }

            std::size_t shroudTexels = 0;
            for (int y = 0; y < mapSize; ++y)
            {
                const float fy = (static_cast<float>(y) + 0.5f) * scale - 0.5f;
                const int iy = static_cast<int>(std::floor(fy));
                const float ty = fy - static_cast<float>(iy);
                const std::uint8_t* row0 = sdf.data() + static_cast<std::size_t>(ClampValue(iy, 0, size - 1)) * static_cast<std::size_t>(size);
                const std::uint8_t* row1 = sdf.data() + static_cast<std::size_t>(ClampValue(iy + 1, 0, size - 1)) * static_cast<std::size_t>(size);
                std::uint8_t* dst = map.rgba.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(mapSize) * 4;
                for (int x = 0; x < mapSize; ++x)
                {
                    const std::size_t i0 = static_cast<std::size_t>(x0s[static_cast<std::size_t>(x)]);
                    const std::size_t i1 = static_cast<std::size_t>(x1s[static_cast<std::size_t>(x)]);
                    const float tx = txs[static_cast<std::size_t>(x)];
                    const float top = static_cast<float>(row0[i0]) + (static_cast<float>(row0[i1]) - static_cast<float>(row0[i0])) * tx;
                    const float bottom = static_cast<float>(row1[i0]) + (static_cast<float>(row1[i1]) - static_cast<float>(row1[i0])) * tx;
                    const float value = top + (bottom - top) * ty;
                    const std::uint8_t encoded = static_cast<std::uint8_t>(ClampValue(static_cast<int>(value + 0.5f), 0, 255));
                    dst[static_cast<std::size_t>(x) * 4 + 3] = encoded;
                    if (encoded < 128)
                        ++shroudTexels;
                }
            }

            std::ostringstream oss;
            oss << "[Minimap] loaded shroud overlay"
                << " | path=" << path
                << " | size=" << size << "x" << size
                << " | shroud_share=" << (100.0 * static_cast<double>(shroudTexels) / (static_cast<double>(mapSize) * static_cast<double>(mapSize))) << "%";
            Log(oss.str());
            return true;
        }

        Log("[Minimap] shroud overlay file missing (embervale_shroud_sdf.r8); shroud areas are not drawn");
        return false;
    }

    // Halves the loaded map until it is no larger than "map_texture_size". Averaging
    // the alpha channel is fine: it holds the shroud signed distance.
    void ShrinkRealMapToConfiguredSize(RealMapTexture& map)
    {
        const int target = g_minimapMapTextureSize.load();
        while (map.loaded && map.width == map.height && map.width > target && (map.width % 2) == 0 && map.width >= 128)
        {
            const std::uint32_t srcSize = static_cast<std::uint32_t>(map.width);
            const std::uint32_t dstSize = srcSize / 2;
            std::vector<std::uint8_t> smaller(static_cast<std::size_t>(dstSize) * dstSize * 4);
            DownsampleRgbaLevel(map.rgba.data(), srcSize, smaller.data(), dstSize);
            map.rgba.swap(smaller);
            map.width = static_cast<int>(dstSize);
            map.height = static_cast<int>(dstSize);
        }
    }

    // The CPU copy (268 MB at 8192) is only needed to fill the GPU staging buffer and
    // for the CPU fallback renderer. Once the upload has finished it is dropped; a later
    // renderer rebuild (resolution change) simply reloads it from disk.
    void ReleaseRealMapCpuCopy()
    {
        std::lock_guard<std::mutex> lock(g_realMapMutex);
        if (!g_realMap.loaded || g_realMap.rgba.empty())
            return;
        std::vector<std::uint8_t>().swap(g_realMap.rgba);
        g_realMap.loaded = false;
        g_realMap.attempted = false;
        if (g_debugLoggingEnabled.load())
            Log("[Minimap] released CPU map copy after GPU upload");
    }

    void EnsureRealMapLoaded()
    {
        std::lock_guard<std::mutex> lock(g_realMapMutex);
        if (g_realMap.attempted)
            return;

        g_realMap.attempted = true;
        g_realMap.loaded = false;
        std::vector<std::uint8_t>().swap(g_realMap.rgba);

        std::vector<std::string> directories;
        if (g_modContext != nullptr && !g_modContext->shroudtopia.mod_folder.empty())
        {
            const std::string modRoot = JoinPath(g_modContext->shroudtopia.mod_folder, "minimap_mod");
            directories.push_back(modRoot);
            directories.push_back(JoinPath(modRoot, "assets"));
        }

        const std::string gameDir = GetExecutableDirectory();
        if (!gameDir.empty())
        {
            const std::string installedModRoot = JoinPath(JoinPath(JoinPath(gameDir, "mods"), "minimap_mod"), "");
            directories.push_back(installedModRoot);
            directories.push_back(JoinPath(installedModRoot, "assets"));
        }

        directories.push_back("");

        for (const std::string& directory : directories)
        {
            if (TryLoadTiledRealMap(directory, g_realMap))
            {
                std::ostringstream oss;
                oss << "[Minimap] loaded HD Embervale map texture"
                    << " | path=" << g_realMap.path
                    << " | size=" << g_realMap.width << "x" << g_realMap.height;
                Log(oss.str());
                TryApplyShroudSdf(directories, g_realMap);
                ShrinkRealMapToConfiguredSize(g_realMap);
                return;
            }
        }

        std::vector<std::string> candidates;
        for (const std::string& directory : directories)
            AddRealMapCandidates(candidates, directory);

        for (const std::string& path : candidates)
        {
            if (TryLoadRealMapFromPath(path, g_realMap))
            {
                std::ostringstream oss;
                oss << "[Minimap] loaded real Embervale map texture"
                    << " | path=" << path
                    << " | size=" << g_realMap.width << "x" << g_realMap.height
                    << " | source=UiMapResource 01bcbd07-bdbf-41c0-9999-5d58fb1a3aa1";
                Log(oss.str());
                TryApplyShroudSdf(directories, g_realMap);
                ShrinkRealMapToConfiguredSize(g_realMap);
                return;
            }
        }

        Log("[Minimap] real Embervale map texture missing; fake procedural map is disabled");
    }

    bool TryLoadStaticPoisFromPath(const std::string& path, StaticPoiCatalog& catalog)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file)
            return false;

        const std::streamoff size = file.tellg();
        if (size < 12)
            return false;

        file.seekg(0, std::ios::beg);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
            return false;

        const char expectedMagicV1[8] = { 'E', 'M', 'P', 'O', 'I', '0', '0', '1' };
        const char expectedMagicV2[8] = { 'E', 'M', 'P', 'O', 'I', '0', '0', '2' };
        if (std::memcmp(bytes.data(), expectedMagicV1, sizeof(expectedMagicV1)) != 0 &&
            std::memcmp(bytes.data(), expectedMagicV2, sizeof(expectedMagicV2)) != 0)
        {
            return false;
        }

        std::uint32_t count = 0;
        std::memcpy(&count, bytes.data() + 8, sizeof(count));
        if (count == 0 || count > STATIC_POI_MAX_ENTRIES)
            return false;

        const std::size_t expectedSize = 12 + static_cast<std::size_t>(count) * STATIC_POI_ENTRY_SIZE;
        if (bytes.size() != expectedSize)
            return false;

        std::vector<StaticPoi> pois;
        pois.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index)
        {
            const std::uint8_t* entry = bytes.data() + 12 + static_cast<std::size_t>(index) * STATIC_POI_ENTRY_SIZE;
            StaticPoi poi{};
            std::memcpy(&poi.x, entry + 0, sizeof(poi.x));
            std::memcpy(&poi.y, entry + 4, sizeof(poi.y));
            std::memcpy(&poi.z, entry + 8, sizeof(poi.z));
            std::memcpy(&poi.kind, entry + 12, sizeof(poi.kind));

            if (!std::isfinite(poi.x) || !std::isfinite(poi.y) || !std::isfinite(poi.z))
                continue;

            if (poi.x < -512.0f || poi.z < -512.0f ||
                poi.x > REAL_MAP_WORLD_SIZE + 768.0f ||
                poi.z > REAL_MAP_WORLD_SIZE + 768.0f)
            {
                continue;
            }

            pois.push_back(poi);
        }

        if (pois.empty())
            return false;

        catalog.loaded = true;
        catalog.path = path;
        catalog.pois = std::move(pois);
        return true;
    }

    void EnsureStaticPoisLoaded()
    {
        std::lock_guard<std::mutex> lock(g_staticPoiMutex);
        if (g_staticPois.attempted)
            return;

        g_staticPois.attempted = true;

        std::vector<std::string> candidates;
        if (g_modContext != nullptr && !g_modContext->shroudtopia.mod_folder.empty())
        {
            const std::string modRoot = JoinPath(g_modContext->shroudtopia.mod_folder, "minimap_mod");
            candidates.push_back(JoinPath(modRoot, "embervale_pois.bin"));
            candidates.push_back(JoinPath(JoinPath(modRoot, "assets"), "embervale_pois.bin"));
        }

        const std::string gameDir = GetExecutableDirectory();
        if (!gameDir.empty())
        {
            candidates.push_back(JoinPath(JoinPath(JoinPath(gameDir, "mods"), "minimap_mod"), "embervale_pois.bin"));
            candidates.push_back(JoinPath(JoinPath(JoinPath(JoinPath(gameDir, "mods"), "minimap_mod"), "assets"), "embervale_pois.bin"));
        }

        candidates.push_back("embervale_pois.bin");

        for (const std::string& path : candidates)
        {
            if (TryLoadStaticPoisFromPath(path, g_staticPois))
            {
                std::ostringstream oss;
                oss << "[Minimap] loaded real Embervale POI catalog"
                    << " | path=" << path
                    << " | count=" << g_staticPois.pois.size()
                    << " | source=SceneResource/MapMarker asset extraction";
                Log(oss.str());
                return;
            }
        }

        Log("[Minimap] real Embervale POI catalog missing; runtime marker feed only");
    }

    bool TryLoadFogOfWarFromPath(const std::string& path, FogOfWarMask& mask)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file)
            return false;

        const std::streamoff size = file.tellg();
        if (size < 16)
            return false;

        file.seekg(0, std::ios::beg);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
            return false;

        const char expectedMagic[8] = { 'E', 'M', 'F', 'O', 'W', '0', '0', '1' };
        if (std::memcmp(bytes.data(), expectedMagic, sizeof(expectedMagic)) != 0)
            return false;

        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::memcpy(&width, bytes.data() + 8, sizeof(width));
        std::memcpy(&height, bytes.data() + 12, sizeof(height));
        if (width < 16 || height < 16 || width > 2048 || height > 2048)
            return false;

        const std::size_t pixelCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
        if (bytes.size() != 16 + pixelCount)
            return false;

        mask.attempted = true;
        mask.loaded = true;
        mask.width = static_cast<int>(width);
        mask.height = static_cast<int>(height);
        mask.path = path;
        mask.values.assign(bytes.begin() + 16, bytes.end());
        return true;
    }

    void EnsureFogOfWarLoaded()
    {
        std::lock_guard<std::mutex> lock(g_fogOfWarMutex);
        if (g_fogOfWar.attempted)
            return;

        g_fogOfWar.attempted = true;

        std::vector<std::string> candidates;
        if (g_modContext != nullptr && !g_modContext->shroudtopia.mod_folder.empty())
        {
            const std::string modRoot = JoinPath(g_modContext->shroudtopia.mod_folder, "minimap_mod");
            candidates.push_back(JoinPath(modRoot, "embervale_fowr.bin"));
            candidates.push_back(JoinPath(JoinPath(modRoot, "assets"), "embervale_fowr.bin"));
        }

        const std::string gameDir = GetExecutableDirectory();
        if (!gameDir.empty())
        {
            candidates.push_back(JoinPath(JoinPath(JoinPath(gameDir, "mods"), "minimap_mod"), "embervale_fowr.bin"));
            candidates.push_back(JoinPath(JoinPath(JoinPath(JoinPath(gameDir, "mods"), "minimap_mod"), "assets"), "embervale_fowr.bin"));
        }

        candidates.push_back("embervale_fowr.bin");

        for (const std::string& path : candidates)
        {
            if (TryLoadFogOfWarFromPath(path, g_fogOfWar))
            {
                std::ostringstream oss;
                oss << "[Minimap] loaded Embervale fog-of-war mask"
                    << " | path=" << path
                    << " | size=" << g_fogOfWar.width << "x" << g_fogOfWar.height
                    << " | source=FOWR save blob";
                Log(oss.str());
                return;
            }
        }

        Log("[Minimap] fog-of-war mask missing; static POI visibility cannot be save-filtered");
    }

    bool IsWorldPointRevealedByFogOfWar(float worldX, float worldZ)
    {
        {
            std::lock_guard<std::mutex> lock(g_fogGridMutex);
            const FogOfWarGrid& grid = g_fogGrid;
            if (grid.valid && grid.width > 0 && grid.height > 0 && grid.cells.size() >= static_cast<std::size_t>(grid.width) * grid.height)
            {
                const float cellX = grid.sizeX / static_cast<float>(grid.width);
                const float cellZ = grid.sizeZ / static_cast<float>(grid.height);
                const int baseX = ClampValue(static_cast<int>(worldX / cellX), 0, static_cast<int>(grid.width) - 1);
                const int baseY = ClampValue(static_cast<int>((grid.sizeZ - worldZ) / cellZ), 0, static_cast<int>(grid.height) - 1);
                std::uint8_t best = 0;
                for (int dy = -1; dy <= 1; ++dy)
                {
                    const int y = ClampValue(baseY + dy, 0, static_cast<int>(grid.height) - 1);
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        const int x = ClampValue(baseX + dx, 0, static_cast<int>(grid.width) - 1);
                        best = MaxValue(best, grid.cells[static_cast<std::size_t>(y) * grid.width + static_cast<std::size_t>(x)]);
                    }
                }
                return best >= 40;
            }
        }

        EnsureFogOfWarLoaded();

        std::lock_guard<std::mutex> lock(g_fogOfWarMutex);
        if (!g_fogOfWar.loaded || g_fogOfWar.values.empty() || g_fogOfWar.width <= 0 || g_fogOfWar.height <= 0)
            return true;

        const int baseX = ClampValue(static_cast<int>((worldX / REAL_MAP_WORLD_SIZE) * static_cast<float>(g_fogOfWar.width)), 0, g_fogOfWar.width - 1);
        const int baseY = ClampValue(static_cast<int>((worldZ / REAL_MAP_WORLD_SIZE) * static_cast<float>(g_fogOfWar.height)), 0, g_fogOfWar.height - 1);

        std::uint8_t best = 0;
        for (int dy = -1; dy <= 1; ++dy)
        {
            const int y = ClampValue(baseY + dy, 0, g_fogOfWar.height - 1);
            for (int dx = -1; dx <= 1; ++dx)
            {
                const int x = ClampValue(baseX + dx, 0, g_fogOfWar.width - 1);
                const std::size_t offset = static_cast<std::size_t>(y) * static_cast<std::size_t>(g_fogOfWar.width) + static_cast<std::size_t>(x);
                best = MaxValue(best, g_fogOfWar.values[offset]);
            }
        }

        return best >= 12;
    }

    void AddMinimapIconCandidates(std::vector<std::string>& candidates, const std::string& directory)
    {
        candidates.push_back(JoinPath(directory, "embervale_minimap_icons.bin"));
    }

    bool TryLoadMinimapIconsFromPath(const std::string& path, MinimapIconAtlas& atlas)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file)
            return false;

        const std::streamoff fileSize = file.tellg();
        if (fileSize < 12)
            return false;

        file.seekg(0, std::ios::beg);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(fileSize));
        if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
            return false;

        const char expectedMagic[8] = { 'E', 'M', 'I', 'C', 'O', '0', '0', '1' };
        if (std::memcmp(bytes.data(), expectedMagic, sizeof(expectedMagic)) != 0)
            return false;

        std::uint32_t count = 0;
        std::memcpy(&count, bytes.data() + 8, sizeof(count));
        if (count == 0 || count > MINIMAP_ICON_MAX_ENTRIES)
            return false;

        std::size_t cursor = 12;
        std::vector<MinimapIcon> icons;
        icons.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index)
        {
            if (cursor + 12 > bytes.size())
                return false;

            MinimapIcon icon{};
            std::memcpy(&icon.key, bytes.data() + cursor + 0, sizeof(icon.key));
            std::memcpy(&icon.width, bytes.data() + cursor + 4, sizeof(icon.width));
            std::memcpy(&icon.height, bytes.data() + cursor + 8, sizeof(icon.height));
            cursor += 12;

            if (icon.width < MINIMAP_ICON_MIN_SIZE || icon.width > MINIMAP_ICON_MAX_SIZE ||
                icon.height < MINIMAP_ICON_MIN_SIZE || icon.height > MINIMAP_ICON_MAX_SIZE)
            {
                return false;
            }

            const std::size_t pixelBytes =
                static_cast<std::size_t>(icon.width) *
                static_cast<std::size_t>(icon.height) *
                4u;
            if (pixelBytes == 0 || cursor + pixelBytes > bytes.size())
                return false;

            icon.rgba.assign(bytes.begin() + static_cast<std::ptrdiff_t>(cursor), bytes.begin() + static_cast<std::ptrdiff_t>(cursor + pixelBytes));
            cursor += pixelBytes;
            icons.push_back(std::move(icon));
        }

        if (cursor != bytes.size() || icons.empty())
            return false;

        atlas.loaded = true;
        atlas.path = path;
        atlas.icons = std::move(icons);
        return true;
    }

    void EnsureMinimapIconsLoaded()
    {
        std::lock_guard<std::mutex> lock(g_minimapIconMutex);
        if (g_minimapIcons.attempted)
            return;

        g_minimapIcons.attempted = true;

        std::vector<std::string> candidates;
        if (g_modContext != nullptr && !g_modContext->shroudtopia.mod_folder.empty())
        {
            const std::string modRoot = JoinPath(g_modContext->shroudtopia.mod_folder, "minimap_mod");
            AddMinimapIconCandidates(candidates, modRoot);
            AddMinimapIconCandidates(candidates, JoinPath(modRoot, "assets"));
        }

        const std::string gameDir = GetExecutableDirectory();
        if (!gameDir.empty())
        {
            const std::string installedModRoot = JoinPath(JoinPath(JoinPath(gameDir, "mods"), "minimap_mod"), "");
            AddMinimapIconCandidates(candidates, installedModRoot);
            AddMinimapIconCandidates(candidates, JoinPath(installedModRoot, "assets"));
        }

        AddMinimapIconCandidates(candidates, "");

        for (const std::string& path : candidates)
        {
            if (TryLoadMinimapIconsFromPath(path, g_minimapIcons))
            {
                std::ostringstream oss;
                oss << "[Minimap] loaded real map marker icon atlas"
                    << " | path=" << path
                    << " | count=" << g_minimapIcons.icons.size()
                    << " | source=MapMarkerRegistryResource/UiTextureResource";
                Log(oss.str());
                return;
            }
        }

        Log("[Minimap] real marker icon atlas missing; legacy primitive marker fallback remains active");
    }

    bool TryResolveFrameRgbaDimensions(std::streamoff byteSize, int& outTextureSize)
    {
        if (byteSize <= 0 || (byteSize % 4) != 0)
            return false;

        const std::uint64_t pixelCount = static_cast<std::uint64_t>(byteSize / 4);
        const int textureSize = static_cast<int>(std::sqrt(static_cast<double>(pixelCount)) + 0.5);
        if (textureSize < MINIMAP_FRAME_MIN_SIZE || textureSize > MINIMAP_FRAME_MAX_SIZE)
            return false;

        if (static_cast<std::uint64_t>(textureSize) * static_cast<std::uint64_t>(textureSize) != pixelCount)
            return false;

        outTextureSize = textureSize;
        return true;
    }

    void AddMinimapFrameCandidates(std::vector<std::string>& candidates, const std::string& directory)
    {
        const char* names[] = {
            "embervale_minimap_frame.rgba",
            "embervale_minimap_frame_512.rgba",
            "embervale_minimap_frame_384.rgba",
            "embervale_minimap_frame_320.rgba"
        };

        for (const char* name : names)
            candidates.push_back(JoinPath(directory, name));
    }

    bool TryLoadMinimapFrameFromPath(const std::string& path, MinimapFrameAsset& asset)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file)
            return false;

        const std::streamoff fileSize = file.tellg();
        int textureSize = 0;
        if (!TryResolveFrameRgbaDimensions(fileSize, textureSize))
            return false;

        file.seekg(0, std::ios::beg);
        std::vector<std::uint8_t> rgba(static_cast<std::size_t>(fileSize));
        if (!file.read(reinterpret_cast<char*>(rgba.data()), rgba.size()))
            return false;

        constexpr int kColorLevels = 64;
        constexpr int kColorMapSize = kColorLevels * kColorLevels * kColorLevels;
        std::vector<int> colorToGroup(kColorMapSize, -1);
        std::vector<MinimapFrameColorGroup> groups;
        groups.reserve(96);

        const auto quantize = [](std::uint8_t value) -> std::uint8_t
        {
            return static_cast<std::uint8_t>(value >> 2);
        };
        const auto expand = [](std::uint8_t value) -> std::uint8_t
        {
            return static_cast<std::uint8_t>((static_cast<unsigned int>(value) * 255u + 31u) / 63u);
        };
        const auto makeKey = [](std::uint8_t red, std::uint8_t green, std::uint8_t blue) -> int
        {
            return (static_cast<int>(red) << 12) | (static_cast<int>(green) << 6) | static_cast<int>(blue);
        };
        const auto appendRun = [&](int y, int startX, int endX, int colorKey)
        {
            if (endX <= startX || colorKey < 0 || colorKey >= kColorMapSize)
                return;

            int groupIndex = colorToGroup[static_cast<std::size_t>(colorKey)];
            if (groupIndex < 0)
            {
                const std::uint8_t red = static_cast<std::uint8_t>((colorKey >> 12) & 0x3F);
                const std::uint8_t green = static_cast<std::uint8_t>((colorKey >> 6) & 0x3F);
                const std::uint8_t blue = static_cast<std::uint8_t>(colorKey & 0x3F);

                MinimapFrameColorGroup group{};
                group.red = expand(red);
                group.green = expand(green);
                group.blue = expand(blue);
                groups.push_back(std::move(group));
                groupIndex = static_cast<int>(groups.size() - 1);
                colorToGroup[static_cast<std::size_t>(colorKey)] = groupIndex;
            }

            MinimapFrameRun run{};
            run.x = static_cast<std::uint16_t>(startX);
            run.y = static_cast<std::uint16_t>(y);
            run.width = static_cast<std::uint16_t>(endX - startX);
            groups[static_cast<std::size_t>(groupIndex)].runs.push_back(run);
        };

        for (int y = 0; y < textureSize; ++y)
        {
            int runStart = -1;
            int runKey = -1;
            for (int x = 0; x < textureSize; ++x)
            {
                const std::size_t offset =
                    (static_cast<std::size_t>(y) * static_cast<std::size_t>(textureSize) + static_cast<std::size_t>(x)) *
                    4u;
                const std::uint8_t alpha = rgba[offset + 3];
                if (alpha <= 40)
                {
                    appendRun(y, runStart, x, runKey);
                    runStart = -1;
                    runKey = -1;
                    continue;
                }

                const int colorKey = makeKey(
                    quantize(rgba[offset + 0]),
                    quantize(rgba[offset + 1]),
                    quantize(rgba[offset + 2]));

                if (runStart >= 0 && runKey == colorKey)
                    continue;

                appendRun(y, runStart, x, runKey);
                runStart = x;
                runKey = colorKey;
            }
            appendRun(y, runStart, textureSize, runKey);
        }

        if (groups.empty())
            return false;

        asset.loaded = true;
        asset.width = textureSize;
        asset.height = textureSize;
        asset.path = path;
        asset.rgba = std::move(rgba);
        asset.groups = std::move(groups);
        return true;
    }

    void EnsureMinimapFrameLoaded()
    {
        std::lock_guard<std::mutex> lock(g_minimapFrameMutex);
        if (g_minimapFrame.attempted)
            return;

        g_minimapFrame.attempted = true;

        std::vector<std::string> candidates;
        if (g_modContext != nullptr && !g_modContext->shroudtopia.mod_folder.empty())
        {
            const std::string modRoot = JoinPath(g_modContext->shroudtopia.mod_folder, "minimap_mod");
            AddMinimapFrameCandidates(candidates, modRoot);
            AddMinimapFrameCandidates(candidates, JoinPath(modRoot, "assets"));
        }

        const std::string gameDir = GetExecutableDirectory();
        if (!gameDir.empty())
        {
            const std::string installedModRoot = JoinPath(JoinPath(JoinPath(gameDir, "mods"), "minimap_mod"), "");
            AddMinimapFrameCandidates(candidates, installedModRoot);
            AddMinimapFrameCandidates(candidates, JoinPath(installedModRoot, "assets"));
        }

        AddMinimapFrameCandidates(candidates, "");

        for (const std::string& path : candidates)
        {
            if (TryLoadMinimapFrameFromPath(path, g_minimapFrame))
            {
                std::size_t runCount = 0;
                for (const MinimapFrameColorGroup& group : g_minimapFrame.groups)
                    runCount += group.runs.size();

                std::ostringstream oss;
                oss << "[Minimap] loaded raster minimap frame"
                    << " | path=" << path
                    << " | size=" << g_minimapFrame.width << "x" << g_minimapFrame.height
                    << " | colors=" << g_minimapFrame.groups.size()
                    << " | runs=" << runCount;
                Log(oss.str());
                return;
            }
        }

        Log("[Minimap] raster minimap frame missing; using vector fallback frame");
    }

    bool HasLoadedMinimapFrame()
    {
        EnsureMinimapFrameLoaded();
        std::lock_guard<std::mutex> lock(g_minimapFrameMutex);
        return g_minimapFrame.loaded;
    }

    bool HasLoadedMinimapIconAtlas()
    {
        std::lock_guard<std::mutex> lock(g_minimapIconMutex);
        return g_minimapIcons.loaded;
    }

    bool TryCopyMinimapIconExact(std::uint32_t key, MinimapIcon& outIcon)
    {
        EnsureMinimapIconsLoaded();

        std::lock_guard<std::mutex> lock(g_minimapIconMutex);
        if (!g_minimapIcons.loaded)
            return false;

        for (const MinimapIcon& icon : g_minimapIcons.icons)
        {
            if (icon.key == key)
            {
                outIcon = icon;
                return true;
            }
        }

        return false;
    }

    std::uint32_t ResolveMinimapIconKey(std::uint32_t key)
    {
        return key;
    }

    bool TryCopyMinimapIcon(std::uint32_t key, MinimapIcon& outIcon)
    {
        const std::uint32_t resolvedKey = ResolveMinimapIconKey(key);
        if (TryCopyMinimapIconExact(resolvedKey, outIcon))
            return true;

        return resolvedKey != key && TryCopyMinimapIconExact(key, outIcon);
    }

    // CPU fallback for map icons (the GPU sprite path is preferred): the game icon in
    // its own colors, nearest-sampled at the minimap icon size, alpha-tested.
    bool TryDrawMinimapRasterIcon(VulkanMinimapRenderer& renderer, void* commandBuffer, int cx, int cy, int radius, int x, int y, std::uint32_t kind, bool clipped)
    {
        MinimapIcon icon{};
        if (!TryCopyMinimapIcon(kind, icon) || icon.width == 0 || icon.height == 0)
            return false;

        const int baseSize = ClampValue(radius / 5, 22, 30);
        const int targetSize = clipped ? (baseSize * 3) / 4 : baseSize;
        const int targetHeight = targetSize;
        const int targetWidth = MaxValue(1, static_cast<int>((static_cast<std::int64_t>(targetSize) * icon.width) / icon.height));
        const int left = x - targetWidth / 2;
        const int top = y - targetHeight / 2;

        for (int dstY = 0; dstY < targetHeight; ++dstY)
        {
            const int screenY = top + dstY;
            const int dy = screenY - cy;
            const std::uint32_t srcY = MinValue<std::uint32_t>(
                icon.height - 1u,
                static_cast<std::uint32_t>(((static_cast<std::uint64_t>(dstY) * 2 + 1) * icon.height) / (static_cast<std::uint64_t>(targetHeight) * 2)));
            int runStart = -1;
            std::uint32_t runColor = 0;

            const auto flushRun = [&](int endX)
            {
                if (runStart < 0 || endX <= runStart)
                    return;

                CmdClearRect(
                    renderer,
                    commandBuffer,
                    static_cast<float>((runColor >> 16) & 0xFF) / 255.0f,
                    static_cast<float>((runColor >> 8) & 0xFF) / 255.0f,
                    static_cast<float>(runColor & 0xFF) / 255.0f,
                    1.0f,
                    runStart,
                    screenY,
                    endX - runStart,
                    1);
                runStart = -1;
            };

            for (int dstX = 0; dstX < targetWidth; ++dstX)
            {
                const int screenX = left + dstX;
                const int dx = screenX - cx;
                if (!IsInsideMinimapWindow(dx, dy, radius))
                {
                    flushRun(screenX);
                    continue;
                }

                const std::uint32_t srcX = MinValue<std::uint32_t>(
                    icon.width - 1u,
                    static_cast<std::uint32_t>(((static_cast<std::uint64_t>(dstX) * 2 + 1) * icon.width) / (static_cast<std::uint64_t>(targetWidth) * 2)));
                const std::size_t offset =
                    (static_cast<std::size_t>(srcY) * static_cast<std::size_t>(icon.width) + static_cast<std::size_t>(srcX)) * 4u;
                if (icon.rgba[offset + 3] < 128)
                {
                    flushRun(screenX);
                    continue;
                }

                // 5-bit color steps keep runs long.
                const std::uint32_t color =
                    (static_cast<std::uint32_t>(icon.rgba[offset] & 0xF8) << 16) |
                    (static_cast<std::uint32_t>(icon.rgba[offset + 1] & 0xF8) << 8) |
                    static_cast<std::uint32_t>(icon.rgba[offset + 2] & 0xF8);
                if (runStart >= 0 && color != runColor)
                    flushRun(screenX);
                if (runStart < 0)
                {
                    runStart = screenX;
                    runColor = color;
                }
            }

            flushRun(left + targetWidth);
        }

        return true;
    }

    bool SampleRealMapColor(float worldX, float worldZ, float& red, float& green, float& blue)
    {
        std::lock_guard<std::mutex> lock(g_realMapMutex);
        if (!g_realMap.loaded || g_realMap.rgba.empty())
            return false;

        const float u = ClampValue(worldX / REAL_MAP_WORLD_SIZE, 0.0f, 1.0f);
        const float v = ClampValue(1.0f - (worldZ / REAL_MAP_WORLD_SIZE), 0.0f, 1.0f);
        const int x = ClampValue(static_cast<int>(u * static_cast<float>(g_realMap.width - 1)), 0, g_realMap.width - 1);
        const int y = ClampValue(static_cast<int>(v * static_cast<float>(g_realMap.height - 1)), 0, g_realMap.height - 1);
        const std::size_t offset = (static_cast<std::size_t>(y) * static_cast<std::size_t>(g_realMap.width) + static_cast<std::size_t>(x)) * 4;

        red = static_cast<float>(g_realMap.rgba[offset + 0]) / 255.0f;
        green = static_cast<float>(g_realMap.rgba[offset + 1]) / 255.0f;
        blue = static_cast<float>(g_realMap.rgba[offset + 2]) / 255.0f;
        return true;
    }

    struct MinimapWorldPoint
    {
        float x = 0.0f;
        float z = 0.0f;
        std::uint32_t kind = 0;
        std::uint32_t label = 0;       // name sprite key, 0 = none
        std::uint32_t highlight = 0;   // yellow waypoint outline sprite, 0 = none
    };

    bool TryResolveKnownMarkerIconKey(std::uint32_t candidate, std::uint32_t& outKey)
    {
        if (candidate <= 0xFFFFu)
            return false;

        EnsureMinimapIconsLoaded();

        std::lock_guard<std::mutex> lock(g_minimapIconMutex);
        if (!g_minimapIcons.loaded)
            return false;

        for (const MinimapIcon& icon : g_minimapIcons.icons)
        {
            if (icon.key == candidate)
            {
                outKey = candidate;
                return true;
            }
        }

        return false;
    }

    bool IsKnownMarkerIconKeyLocked(std::uint32_t candidate)
    {
        if (candidate <= 0xFFFFu || !g_minimapIcons.loaded)
            return false;

        for (const MinimapIcon& icon : g_minimapIcons.icons)
        {
            if (icon.key == candidate)
                return true;
        }

        return false;
    }

    bool IsSpecialMapMarkerKind(std::uint32_t candidate)
    {
        return candidate == MAP_MARKER_QUEST_IMPORTANT_KIND ||
            candidate == MAP_MARKER_QUEST_KIND;
    }

    bool TryReadMapMarkerTypeFromRawWords(const std::uint64_t* words, std::size_t wordCount, std::uint32_t& outKind)
    {
        if (words == nullptr || wordCount < 3)
            return false;

        outKind = static_cast<std::uint32_t>(words[2] & 0xFFFFFFFFu);
        return outKind != 0;
    }

    std::uint32_t ResolveIconKeyFromRawWordsIfAtlasLoaded(const std::uint64_t* words, std::size_t wordCount, std::uint32_t fallback)
    {
        std::lock_guard<std::mutex> lock(g_minimapIconMutex);
        if (!g_minimapIcons.loaded)
        {
            std::uint32_t markerType = 0;
            if (TryReadMapMarkerTypeFromRawWords(words, wordCount, markerType) && IsSpecialMapMarkerKind(markerType))
                return markerType;
            return fallback;
        }

        std::uint32_t markerType = 0;
        if (TryReadMapMarkerTypeFromRawWords(words, wordCount, markerType) &&
            (IsSpecialMapMarkerKind(markerType) || IsKnownMarkerIconKeyLocked(markerType)))
        {
            return markerType;
        }

        if (IsKnownMarkerIconKeyLocked(fallback))
            return fallback;

        for (std::size_t index = 0; index < wordCount; ++index)
        {
            const std::uint32_t low = static_cast<std::uint32_t>(words[index] & 0xFFFFFFFFu);
            const std::uint32_t high = static_cast<std::uint32_t>((words[index] >> 32u) & 0xFFFFFFFFu);
            if (IsKnownMarkerIconKeyLocked(low))
                return low;
            if (IsKnownMarkerIconKeyLocked(high))
                return high;
        }

        return fallback;
    }

    std::uint32_t ResolveIconKeyFromRawWords(const std::uint64_t* words, std::size_t wordCount, std::uint32_t fallback)
    {
        EnsureMinimapIconsLoaded();

        std::lock_guard<std::mutex> lock(g_minimapIconMutex);
        if (!g_minimapIcons.loaded)
        {
            std::uint32_t markerType = 0;
            if (TryReadMapMarkerTypeFromRawWords(words, wordCount, markerType) && IsSpecialMapMarkerKind(markerType))
                return markerType;
            return fallback;
        }

        std::uint32_t markerType = 0;
        if (TryReadMapMarkerTypeFromRawWords(words, wordCount, markerType) &&
            (IsSpecialMapMarkerKind(markerType) || IsKnownMarkerIconKeyLocked(markerType)))
        {
            return markerType;
        }

        if (IsKnownMarkerIconKeyLocked(fallback))
            return fallback;

        for (std::size_t index = 0; index < wordCount; ++index)
        {
            const std::uint32_t low = static_cast<std::uint32_t>(words[index] & 0xFFFFFFFFu);
            const std::uint32_t high = static_cast<std::uint32_t>((words[index] >> 32u) & 0xFFFFFFFFu);
            if (IsKnownMarkerIconKeyLocked(low))
                return low;
            if (IsKnownMarkerIconKeyLocked(high))
                return high;
        }

        return fallback;
    }

    // Height of the padded silhouette relative to the icon it outlines.
    float IconSilhouetteDrawScale(std::uint32_t iconKey)
    {
        std::lock_guard<std::mutex> lock(g_minimapIconMutex);
        for (const MinimapIcon& icon : g_minimapIcons.icons)
        {
            if (icon.key == iconKey && icon.height != 0)
                return static_cast<float>(icon.height + IconSilhouettePadding(icon.width, icon.height) * 2) / static_cast<float>(icon.height);
        }
        return 1.0f;
    }

    std::uint32_t FindIconSilhouetteKey(std::uint32_t iconKey)
    {
        EnsureMinimapIconsLoaded();
        std::lock_guard<std::mutex> lock(g_minimapIconMutex);
        if (!g_minimapIcons.loaded)
            return 0;
        for (std::size_t index = 0; index < g_minimapIcons.icons.size() && index < 0xFFFFu; ++index)
        {
            if (g_minimapIcons.icons[index].key == iconKey)
                return SPRITE_KEY_SILHOUETTE_BASE + static_cast<std::uint32_t>(index);
        }
        return 0;
    }

    std::uint32_t ResolveCapturedWaypointKind(const CapturedWaypoint& waypoint)
    {
        // Custom map markers carry their own icon (chest, ore, goal...); fall back to the
        // red flag when the type cannot be resolved.
        return ResolveIconKeyFromRawWords(waypoint.raw, sizeof(waypoint.raw) / sizeof(waypoint.raw[0]), 11);
    }

    std::uint32_t ResolveCapturedMarkerKind(const CapturedNearbyMarker& marker)
    {
        return ResolveIconKeyFromRawWords(marker.raw, sizeof(marker.raw) / sizeof(marker.raw[0]), marker.kind);
    }

    std::uint32_t ResolveCapturedVisibleMapMarkerKind(const CapturedMapMarkerVisibility& marker)
    {
        return ResolveIconKeyFromRawWords(marker.raw, sizeof(marker.raw) / sizeof(marker.raw[0]), marker.kind);
    }

    bool TryInitMapMarkerVisibilityRecord(void* ctx, MapMarkerVisibilityIterationRecord& record)
    {
        __try
        {
            alignas(16) std::uint8_t scratch[ITER_RECORD_SCRATCH_BYTES] = {};
            g_iterInit(ctx, scratch, static_cast<std::uint32_t>(sizeof(record)));
            const bool ok = g_iterNext(ctx, scratch, static_cast<std::uint32_t>(sizeof(record)));
            std::memcpy(&record, scratch, sizeof(record));
            return ok;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool IsPlausibleMapMarkerPosition(float x, float y, float z)
    {
        return IsPlausibleWorldPosition(x, y, z) &&
            x >= -512.0f &&
            z >= -512.0f &&
            x <= REAL_MAP_WORLD_SIZE + 768.0f &&
            z <= REAL_MAP_WORLD_SIZE + 768.0f;
    }

    bool TryCaptureMarkerWorldPositionFromBlock(uintptr_t base, std::size_t scanBytes, std::int64_t& outX, std::int64_t& outY, std::int64_t& outZ, std::size_t& bestOffset)
    {
        if (!IsLikelyRuntimePointer(base) || scanBytes < sizeof(std::uint64_t) * 5)
            return false;

        bool found = false;
        for (std::size_t offset = 0; offset + sizeof(std::uint64_t) * 5 <= scanBytes; offset += sizeof(std::uint64_t))
        {
            std::uint64_t raw[5] = {};
            if (!SafeRead(base + offset, raw, sizeof(raw)))
                continue;

            std::int64_t x = 0;
            std::int64_t y = 0;
            std::int64_t z = 0;
            if (!TryDecodeFixedWorldPosition(raw, x, y, z))
                continue;

            if (!IsPlausibleMapMarkerPosition(FixedToWorld(x), FixedToWorld(y), FixedToWorld(z)))
                continue;

            if (!found || offset < bestOffset)
            {
                outX = x;
                outY = y;
                outZ = z;
                bestOffset = offset;
                found = true;
            }
        }

        return found;
    }

    bool TryCaptureMarkerWorldPositionFromPointerChildren(uintptr_t root, std::int64_t& outX, std::int64_t& outY, std::int64_t& outZ, std::size_t& bestOffset)
    {
        if (!IsLikelyRuntimePointer(root))
            return false;

        bool found = false;
        for (std::size_t pointerOffset = 0; pointerOffset < 0x40; pointerOffset += sizeof(uintptr_t))
        {
            uintptr_t child = 0;
            if (!SafeReadValue(root + pointerOffset, child) || !IsLikelyRuntimePointer(child))
                continue;

            std::size_t childOffset = 0;
            if (!TryCaptureMarkerWorldPositionFromBlock(child, 0x100, outX, outY, outZ, childOffset))
                continue;

            const std::size_t combinedOffset = 0x1000 + pointerOffset * 0x100 + childOffset;
            if (!found || combinedOffset < bestOffset)
            {
                bestOffset = combinedOffset;
                found = true;
            }
        }

        return found;
    }

    bool TryResolveVisibleMapMarkerWorldPosition(const MapMarkerVisibilityIterationRecord& record, CapturedMapMarkerVisibility& marker)
    {
        std::size_t bestOffset = static_cast<std::size_t>(-1);
        bool found = false;

        const auto tryRoot = [&](uintptr_t root, std::size_t bytes)
        {
            std::size_t offset = bestOffset;
            std::int64_t x = 0;
            std::int64_t y = 0;
            std::int64_t z = 0;
            if (TryCaptureMarkerWorldPositionFromBlock(root, bytes, x, y, z, offset) && (!found || offset < bestOffset))
            {
                marker.x = x;
                marker.y = y;
                marker.z = z;
                bestOffset = offset;
                found = true;
            }
        };

        const uintptr_t markerAddress = reinterpret_cast<uintptr_t>(record.marker);
        const uintptr_t knowledgeAddress = reinterpret_cast<uintptr_t>(record.knowledge);
        const uintptr_t entityAddress = reinterpret_cast<uintptr_t>(record.entity);
        const uintptr_t unknownAddress = reinterpret_cast<uintptr_t>(record.unknown0);
        const uintptr_t stateAddress = reinterpret_cast<uintptr_t>(record.visibilityState);

        tryRoot(markerAddress, 0x100);
        tryRoot(knowledgeAddress, 0x100);
        tryRoot(entityAddress, 0x180);
        tryRoot(unknownAddress, 0x100);
        tryRoot(stateAddress, 0x60);

        marker.hasWorldPosition = found;
        return found;
    }

    void PruneVisibleMapMarkersLocked(DWORD now)
    {
        if (now - g_lastVisibleMapMarkerPruneTick < VISIBLE_MAP_MARKER_PRUNE_MS)
            return;

        g_lastVisibleMapMarkerPruneTick = now;
        g_visibleMapMarkers.erase(
            std::remove_if(g_visibleMapMarkers.begin(), g_visibleMapMarkers.end(), [now](const CapturedMapMarkerVisibility& marker)
            {
                return now - marker.lastUpdateTick > VISIBLE_MAP_MARKER_STALE_MS;
            }),
            g_visibleMapMarkers.end());
    }

    void PublishVisibleMapMarker(CapturedMapMarkerVisibility&& marker)
    {
        const DWORD now = GetTickCount();
        marker.lastUpdateTick = now;

        std::size_t visibleCount = 0;
        std::size_t positionedCount = 0;
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(g_visibleMapMarkerMutex);
            PruneVisibleMapMarkersLocked(now);

            auto existing = std::find_if(g_visibleMapMarkers.begin(), g_visibleMapMarkers.end(), [&marker](const CapturedMapMarkerVisibility& current)
            {
                return SameVisibleMapMarkerIdentity(current, marker);
            });

            if (existing != g_visibleMapMarkers.end())
            {
                const bool hadPosition = existing->hasWorldPosition;
                if (!marker.hasWorldPosition && hadPosition)
                {
                    marker.x = existing->x;
                    marker.y = existing->y;
                    marker.z = existing->z;
                    marker.hasWorldPosition = true;
                }

                changed = existing->visibility != marker.visibility ||
                    existing->kind != marker.kind ||
                    existing->hasWorldPosition != marker.hasWorldPosition ||
                    existing->x != marker.x ||
                    existing->z != marker.z;
                *existing = marker;
            }
            else if (marker.visibility != 0 && g_visibleMapMarkers.size() < VISIBLE_MAP_MARKER_MAX_ENTRIES)
            {
                g_visibleMapMarkers.push_back(marker);
                changed = true;
            }

            for (const CapturedMapMarkerVisibility& current : g_visibleMapMarkers)
            {
                if (current.visibility == 0 || now - current.lastUpdateTick > VISIBLE_MAP_MARKER_STALE_MS)
                    continue;

                ++visibleCount;
                if (current.hasWorldPosition)
                    ++positionedCount;
            }
        }

        if (!changed || now - g_lastVisibleMapMarkerSummaryTick < 5000)
            return;

        if (!g_debugLoggingEnabled.load())
            return;

        g_lastVisibleMapMarkerSummaryTick = now;
        std::ostringstream oss;
        oss << "[Minimap] real map marker visibility feed"
            << " | visible=" << visibleCount
            << " | positioned=" << positionedCount
            << " | kind=0x" << std::hex << marker.kind << std::dec
            << " | visibility=" << static_cast<int>(marker.visibility);
        if (marker.hasWorldPosition)
        {
            oss << " | world=("
                << FixedToWorld(marker.x) << ", "
                << FixedToWorld(marker.y) << ", "
                << FixedToWorld(marker.z) << ")";
        }
        Log(oss.str());
    }

    bool TryPublishMapMarkerVisibilityRecord(const MapMarkerVisibilityIterationRecord& record)
    {
        if (record.visibilityState == nullptr)
            return false;

        CapturedMapMarkerVisibility marker{};
        marker.stateAddress = reinterpret_cast<uintptr_t>(record.visibilityState);
        marker.entityAddress = reinterpret_cast<uintptr_t>(record.entity);

        // May-24-2026 client moved the marker object from record+0x08 to record+0x20
        // (old +0x20 slot was "knowledge"); pick the first slot whose raw block reads
        // and carries a non-zero id word.
        const uintptr_t markerCandidates[] = {
            reinterpret_cast<uintptr_t>(record.knowledge),
            reinterpret_cast<uintptr_t>(record.marker)
        };

        bool haveMarker = false;
        for (uintptr_t candidate : markerCandidates)
        {
            if (!IsLikelyRuntimePointer(candidate))
                continue;

            std::uint64_t raw[sizeof(marker.raw) / sizeof(marker.raw[0])] = {};
            if (!SafeRead(candidate, raw, sizeof(raw)))
                continue;

            if (static_cast<std::uint32_t>(raw[0] & 0xFFFFFFFFu) == 0)
                continue;

            std::memcpy(marker.raw, raw, sizeof(marker.raw));
            marker.markerAddress = candidate;
            haveMarker = true;
            break;
        }

        if (!haveMarker)
            return false;

        SafeReadValue(marker.stateAddress, marker.visibility);
        marker.markerId = static_cast<std::uint32_t>(marker.raw[0] & 0xFFFFFFFFu);
        TryReadMapMarkerTypeFromRawWords(marker.raw, sizeof(marker.raw) / sizeof(marker.raw[0]), marker.markerType);
        marker.kind = ResolveIconKeyFromRawWordsIfAtlasLoaded(marker.raw, sizeof(marker.raw) / sizeof(marker.raw[0]), marker.markerType != 0 ? marker.markerType : marker.markerId);

        CapturedMapMarkerVisibility existing{};
        bool hasExistingPosition = false;
        {
            std::lock_guard<std::mutex> lock(g_visibleMapMarkerMutex);
            auto existingIt = std::find_if(g_visibleMapMarkers.begin(), g_visibleMapMarkers.end(), [&marker](const CapturedMapMarkerVisibility& current)
            {
                return SameVisibleMapMarkerIdentity(current, marker);
            });
            if (existingIt != g_visibleMapMarkers.end() && existingIt->hasWorldPosition)
            {
                existing = *existingIt;
                hasExistingPosition = true;
            }
        }

        if (hasExistingPosition)
        {
            marker.x = existing.x;
            marker.y = existing.y;
            marker.z = existing.z;
            marker.hasWorldPosition = true;
        }
        else if (marker.visibility != 0)
        {
            TryResolveVisibleMapMarkerWorldPosition(record, marker);
        }

        PublishVisibleMapMarker(std::move(marker));
        g_visibleMapMarkerFaults.store(0, std::memory_order_relaxed);
        return true;
    }

    bool TryCaptureMapMarkerVisibility(void* ctx)
    {
        if (g_iterInit == nullptr || g_iterNext == nullptr || ctx == nullptr)
            return false;

        MapMarkerVisibilityIterationRecord record{};
        if (!TryInitMapMarkerVisibilityRecord(ctx, record))
            return false;

        return TryPublishMapMarkerVisibilityRecord(record);
    }

    void __fastcall CaptureMapMarkerVisibilityHook(void* ctx, void*, void*, void*)
    {
        TryCaptureMapMarkerVisibility(ctx);
    }

    void RegisterVisibleMapMarkerCaptureFault()
    {
        const int faults = g_visibleMapMarkerFaults.fetch_add(1, std::memory_order_relaxed) + 1;
        if (faults >= 3)
            g_visibleMapMarkerCaptureDisabled.store(true, std::memory_order_relaxed);

        const DWORD now = GetTickCount();
        if (now - g_lastVisibleMapMarkerFaultTick < 5000)
            return;

        g_lastVisibleMapMarkerFaultTick = now;
        std::ostringstream oss;
        oss << "[Minimap] map marker visibility capture fault"
            << " | faults=" << faults
            << " | disabled=" << (g_visibleMapMarkerCaptureDisabled.load(std::memory_order_relaxed) ? 1 : 0);
        Log(oss.str());
    }

    void __fastcall CaptureMapMarkerVisibilityRecordHook(MapMarkerVisibilityIterationRecord* recordPtr, void*)
    {
        if (!g_minimapEnabled.load())
            return;
        if (g_visibleMapMarkerCaptureDisabled.load(std::memory_order_relaxed) || recordPtr == nullptr)
            return;

        MapMarkerVisibilityIterationRecord record{};
        bool copied = false;
        __try
        {
            record = *recordPtr;
            copied = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            RegisterVisibleMapMarkerCaptureFault();
            return;
        }

        if (copied)
            TryPublishMapMarkerVisibilityRecord(record);
    }

    bool IsPersonalMarkerKind(std::uint32_t kind)
    {
        return kind == 1 || kind == 2 || kind == 10 || kind == 11 || kind == 12;
    }

    bool IsRuntimeMarkerKind(std::uint32_t kind)
    {
        return kind == 1 || kind == 2 || kind == 3 || kind == 11 || kind == 12 || kind == 13;
    }

    bool IsVisibleMapMarkerDrawable(const CapturedMapMarkerVisibility& marker)
    {
        if (marker.visibility < 2 || !marker.hasWorldPosition)
            return false;

        const std::uint32_t kind = ResolveCapturedVisibleMapMarkerKind(marker);
        if (kind <= 0xFFFFu || IsSpecialMapMarkerKind(kind))
            return true;

        std::uint32_t knownKey = 0;
        return TryResolveKnownMarkerIconKey(kind, knownKey);
    }

    void PushWorldPointUnique(std::vector<MinimapWorldPoint>& points, const MinimapWorldPoint& point)
    {
        for (const MinimapWorldPoint& existing : points)
        {
            const float dx = existing.x - point.x;
            const float dz = existing.z - point.z;
            if (dx * dx + dz * dz < 16.0f)
                return;
        }

        points.push_back(point);
    }

    bool HasVisibleMapMarkerNear(const std::vector<MinimapWorldPoint>& visibleMarkers, float x, float z, std::uint32_t kind);

    // ---- REMOTE PLAYERS (world map player list) ----
    // The world map adds its "playerMarkersLayer" from a player list the map UI reads
    // directly (exe 0x140bd0d60, called from the map screen at 0x140b0146e):
    //   G = *[rip global]; S = *(*(G + 0xC8)) + 0x28 -> session
    //   players = S+0x305550 (ptr) / S+0x305558 (count), 0xF0-byte entries
    //   entry +0x00 player id (local player id at S+0x810), +0x10 float x, +0x18 float z,
    //   +0xA0 name, +0xC6 hidden flag.
    // The offsets are read back from that code so small game updates keep working.
    struct SessionPlayerLayout
    {
        uintptr_t globalRva = 0;
        std::uint32_t rootOffset = 0xC8;
        std::uint8_t sessionOffset = 0x28;
        std::uint32_t countOffset = 0x305558;
        std::uint32_t arrayOffset = 0x305550;
        std::uint32_t hiddenOffset = 0xC6;
        std::uint32_t nameOffset = 0xA0;
        std::uint32_t localIdOffset = 0x810;
        std::uint8_t positionOffset = 0x10;
    };

    constexpr uintptr_t RVA_MAP_PLAYER_LIST_PREFERRED = 0xBD0D7A;
    constexpr std::size_t SESSION_PLAYER_STRIDE = 0xF0;
    constexpr std::size_t SESSION_PLAYER_LEVEL_OFFSET = 0xB8;   // player_ui writes NetworkLevel here
    // player_waypoints_ui (exe 0x1402a8370) copies each player's PlayerWaypoint into the
    // same entry: fixed-point position at +0x30 and "has a waypoint" flag at +0x48.
    // A ping is a waypoint with the ping flag set, so both land here.
    constexpr std::size_t SESSION_PLAYER_WAYPOINT_OFFSET = 0x30;
    constexpr std::size_t SESSION_PLAYER_WAYPOINT_FLAG_OFFSET = 0x48;
    constexpr std::uint32_t MAP_MARKER_KEY_PLAYER_PING = 0x83405288u;   // mapmarker_playerPing
    // Extra textures added to embervale_minimap_icons.bin with tools/map-render/add_icon.py:
    constexpr std::uint32_t MAP_MARKER_KEY_WAYPOINT = 0xFFFF0010u;      // mapmarker_waypoint (64x64)
    constexpr std::uint32_t MAP_MARKER_KEY_NPC_FIGURE = 0xFFFF0011u;    // compass_player figure, drawn sky blue for NPCs
    // player_waypoints_ui also appends UiPingEvent / UiPingInputEvent records (0x28 bytes)
    // to FbUiPlayData+0x3698 (ptr) / +0x36A0 (count).
    constexpr std::size_t UI_PING_EVENT_ARRAY = 0x3698;
    constexpr std::size_t UI_PING_EVENT_STRIDE = 0x28;
    // Record layout seen in the 9/18 logs: +0x00 id, +0x08 kind, +0x10 fixed-point x/y/z.
    constexpr std::size_t UI_PING_EVENT_POSITION_OFFSET = 0x10;
    constexpr std::size_t UI_PING_EVENT_PLAYER_OFFSET = 0x08;
    std::uint64_t HashBytes(const std::uint8_t* data, std::size_t size)
    {
        std::uint64_t hash = 0xCBF29CE484222325ULL;
        for (std::size_t i = 0; i < size; ++i)
        {
            hash ^= data[i];
            hash *= 0x100000001B3ULL;
        }
        return hash;
    }

    constexpr std::size_t PING_EVENT_MAX = 32;
    constexpr std::size_t PING_EVENT_SLOTS = 8;

    struct PingEventMark
    {
        float x = 0.0f;
        float z = 0.0f;
        std::uint32_t playerId = 0;
        DWORD tick = 0;
    };

    std::atomic<std::uint64_t> g_pingEventCount{ 0 };
    std::mutex g_pingEventMutex;
    std::string g_pingEventSample;
    std::array<std::uint64_t, PING_EVENT_SLOTS> g_pingEventHashes{};   // guarded by g_pingEventMutex
    std::vector<PingEventMark> g_pingEvents;                          // guarded by g_pingEventMutex

    std::vector<PingEventMark> CopyPingEvents()
    {
        std::lock_guard<std::mutex> lock(g_pingEventMutex);
        return g_pingEvents;
    }

    struct PlayerWaypointMark
    {
        float x = 0.0f;
        float z = 0.0f;
        DWORD lastSeenTick = 0;
    };
    constexpr std::uint64_t SESSION_PLAYER_MAX = 64;

    std::atomic<int> g_sessionLayoutState{ 0 };   // 0 = not started, 1 = searching, 2 = ready, 3 = failed
    SessionPlayerLayout g_sessionLayout{};        // written once before state becomes 2

    bool MatchWildcard(const std::uint8_t* data, const std::int16_t* pattern, std::size_t size)
    {
        for (std::size_t i = 0; i < size; ++i)
        {
            if (pattern[i] >= 0 && data[i] != static_cast<std::uint8_t>(pattern[i]))
                return false;
        }
        return true;
    }

    bool TryParseSessionPlayerCode(const std::uint8_t* code, uintptr_t codeRva, SessionPlayerLayout& layout)
    {
        // 140bd0d7a: mov rax,[rip+X]; mov rdi,rcx; mov r12,[rax+C8]; mov [rbp+108],r12;
        //            mov r13,[r12]; mov [rbp+100],r13; test r13,r13; jne; xor r14d,r14d; jmp;
        //            mov r14,[r13+28]
        static const std::int16_t head[] = {
            0x48, 0x8B, 0x05, -1, -1, -1, -1,
            0x48, 0x8B, 0xF9,
            0x4C, 0x8B, 0xA0, -1, -1, -1, -1,
            0x4C, 0x89, 0xA5, -1, -1, -1, -1,
            0x4D, 0x8B, 0x2C, 0x24,
            0x4C, 0x89, 0xAD, -1, -1, -1, -1,
            0x4D, 0x85, 0xED, 0x75, 0x05, 0x45, 0x33, 0xF6, 0xEB, 0x04,
            0x4D, 0x8B, 0x75, -1
        };
        if (!MatchWildcard(code, head, sizeof(head) / sizeof(head[0])))
            return false;

        std::int32_t displacement = 0;
        std::memcpy(&displacement, code + 3, sizeof(displacement));
        layout.globalRva = static_cast<uintptr_t>(static_cast<std::intptr_t>(codeRva) + 7 + displacement);
        std::memcpy(&layout.rootOffset, code + 13, sizeof(layout.rootOffset));
        layout.sessionOffset = code[48];

        // +0x43: cmp [r14+count],rsi   +0x66: mov r15,[r14+array]
        // +0x6D: cmp byte [r15+rbx+hidden],0   +0x85: movups xmm1,[r15+rbx+name]
        static const std::int16_t countOp[] = { 0x49, 0x39, 0xB6 };
        static const std::int16_t arrayOp[] = { 0x4D, 0x8B, 0xBE };
        static const std::int16_t hiddenOp[] = { 0x41, 0x80, 0xBC, 0x1F };
        static const std::int16_t nameOp[] = { 0x41, 0x0F, 0x10, 0x8C, 0x1F };
        if (!MatchWildcard(code + 0x43, countOp, 3) || !MatchWildcard(code + 0x66, arrayOp, 3) ||
            !MatchWildcard(code + 0x6D, hiddenOp, 4) || !MatchWildcard(code + 0x85, nameOp, 5))
        {
            return false;
        }
        std::memcpy(&layout.countOffset, code + 0x46, sizeof(layout.countOffset));
        std::memcpy(&layout.arrayOffset, code + 0x69, sizeof(layout.arrayOffset));
        std::memcpy(&layout.hiddenOffset, code + 0x71, sizeof(layout.hiddenOffset));
        std::memcpy(&layout.nameOffset, code + 0x8A, sizeof(layout.nameOffset));
        if (layout.arrayOffset + 8 != layout.countOffset)
            return false;

        // +0xD6: mov ecx,[rax+localId]   +0x141: movups xmm6,[r15+rbx+pos]
        static const std::int16_t localOp[] = { 0x8B, 0x88 };
        static const std::int16_t posOp[] = { 0x41, 0x0F, 0x10, 0x74, 0x1F };
        if (MatchWildcard(code + 0xDC, localOp, 2))
            std::memcpy(&layout.localIdOffset, code + 0xDE, sizeof(layout.localIdOffset));
        if (MatchWildcard(code + 0x141, posOp, 5))
            layout.positionOffset = code[0x146];
        return true;
    }

    void FindSessionPlayerLayout()
    {
        SessionPlayerLayout layout{};
        bool found = false;
        uintptr_t foundRva = 0;
        constexpr std::size_t CODE_BYTES = 0x160;
        constexpr std::size_t CHUNK = 0x100000;
        std::vector<std::uint8_t> buffer(CHUNK + CODE_BYTES);

        const auto scanRange = [&](uintptr_t startRva, uintptr_t endRva)
        {
            for (uintptr_t rva = startRva; rva < endRva && !found; rva += CHUNK)
            {
                const std::size_t bytes = static_cast<std::size_t>(MinValue<uintptr_t>(CHUNK + CODE_BYTES, g_exeImageSize - rva));
                if (bytes < CODE_BYTES || !SafeRead(g_exeBase + rva, buffer.data(), bytes))
                    continue;
                const std::size_t limit = MinValue<std::size_t>(CHUNK, bytes - CODE_BYTES);
                for (std::size_t i = 0; i < limit; ++i)
                {
                    if (buffer[i] != 0x48 || buffer[i + 1] != 0x8B || buffer[i + 2] != 0x05 || buffer[i + 7] != 0x48)
                        continue;
                    if (TryParseSessionPlayerCode(buffer.data() + i, rva + i, layout))
                    {
                        found = true;
                        foundRva = rva + i;
                        break;
                    }
                }
            }
        };

        if (g_exeBase != 0 && g_exeImageSize > 0x2000)
        {
            const uintptr_t nearStart = RVA_MAP_PLAYER_LIST_PREFERRED > 0x200000 ? RVA_MAP_PLAYER_LIST_PREFERRED - 0x200000 : 0x1000;
            const uintptr_t nearEnd = MinValue<uintptr_t>(RVA_MAP_PLAYER_LIST_PREFERRED + 0x200000, g_exeImageSize);
            scanRange(nearStart, nearEnd);
            if (!found)
                scanRange(0x1000, g_exeImageSize);
        }

        std::ostringstream oss;
        if (found && layout.globalRva < g_exeImageSize)
        {
            g_sessionLayout = layout;
            g_sessionLayoutState.store(2);
            oss << "[Minimap] map player list resolved | code=" << Hex(foundRva)
                << " | global=" << Hex(layout.globalRva)
                << " | root=" << Hex(layout.rootOffset) << " session=" << Hex(layout.sessionOffset)
                << " | array=" << Hex(layout.arrayOffset) << " count=" << Hex(layout.countOffset)
                << " | hidden=" << Hex(layout.hiddenOffset) << " name=" << Hex(layout.nameOffset)
                << " local_id=" << Hex(layout.localIdOffset) << " pos=" << Hex(layout.positionOffset);
        }
        else
        {
            g_sessionLayoutState.store(3);
            oss << "[Minimap] map player list code not found; other players will not be shown";
        }
        Log(oss.str());
    }

    struct RemotePlayer
    {
        std::uint32_t id = 0;
        std::uint32_t level = 0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        std::string name;
        DWORD lastSeenTick = 0;
    };

    constexpr DWORD REMOTE_PLAYER_HOLD_MS = 1500;
    std::mutex g_remotePlayerMutex;
    std::vector<RemotePlayer> g_remotePlayers;
    std::vector<PlayerWaypointMark> g_playerWaypoints;
    std::atomic<std::uint64_t> g_remotePlayerReads{ 0 };
    std::atomic<std::uint64_t> g_remotePlayerListSize{ 0 };
    std::atomic<std::uint64_t> g_remotePlayerListMax{ 0 };
    std::atomic<std::uint64_t> g_pingPointsBuilt{ 0 };
    std::atomic<std::uint64_t> g_pingPointsDrawn{ 0 };
    std::atomic<std::uint64_t> g_pingSpriteDrawn{ 0 };
    std::atomic<std::uint32_t> g_localPlayerId{ 0 };
    std::mutex g_localPlayerNameMutex;
    std::string g_localPlayerName;
    std::string g_remotePlayerRawSample;   // guarded by g_remotePlayerMutex

    std::string ReadPlayerNameAt(const std::uint8_t* entry, std::uint32_t nameOffset)
    {
        std::uint64_t words[3] = {};
        std::memcpy(words, entry + nameOffset, sizeof(words));
        // Try (char*, length) at +0 and at +8.
        for (int shift = 0; shift < 2; ++shift)
        {
            const uintptr_t pointer = static_cast<uintptr_t>(words[shift]);
            const std::uint64_t length = words[shift + 1];
            if (length == 0 || length > 64 || !IsLikelyRuntimePointer(pointer))
                continue;
            char buffer[65] = {};
            if (!SafeRead(pointer, buffer, static_cast<std::size_t>(length)))
                continue;
            std::string text(buffer, static_cast<std::size_t>(length));
            bool printable = true;
            for (char c : text)
            {
                if (static_cast<unsigned char>(c) < 0x20)
                    printable = false;
            }
            if (printable)
                return text;
        }
        return std::string();
    }

    void TryCaptureRemotePlayers(std::uint8_t* state)
    {
        int layoutState = g_sessionLayoutState.load();
        if (layoutState == 0)
        {
            int expected = 0;
            if (g_sessionLayoutState.compare_exchange_strong(expected, 1))
                std::thread([]()
                {
                    BackgroundThreadScope scope;
                    FindSessionPlayerLayout();
                }).detach();
            return;
        }
        if (layoutState != 2)
            return;
        g_remotePlayerReads.fetch_add(1, std::memory_order_relaxed);

        // player_ui (exe 0x140266b80) refills this list inside FbUiPlayData every frame,
        // which is the same UI state the rest of the mod reads. The pointer chain the map
        // screen uses ([global+0xC8] -> [] -> +0x28) is only a fallback.
        const SessionPlayerLayout& layout = g_sessionLayout;
        uintptr_t session = reinterpret_cast<uintptr_t>(state);
        if (session == 0)
        {
            uintptr_t global = 0;
            uintptr_t root = 0;
            uintptr_t holder = 0;
            if (!SafeReadValue(g_exeBase + layout.globalRva, global) || !IsLikelyRuntimePointer(global) ||
                !SafeReadValue(global + layout.rootOffset, root) || !IsLikelyRuntimePointer(root) ||
                !SafeReadValue(root, holder) || !IsLikelyRuntimePointer(holder) ||
                !SafeReadValue(holder + layout.sessionOffset, session) || !IsLikelyRuntimePointer(session))
            {
                return;
            }
        }

        uintptr_t entries = 0;
        std::uint64_t count = 0;
        std::uint32_t localId = 0;
        if (!SafeReadValue(session + layout.arrayOffset, entries) ||
            !SafeReadValue(session + layout.countOffset, count) ||
            !SafeReadValue(session + layout.localIdOffset, localId))
        {
            return;
        }
        g_remotePlayerListSize.store(count, std::memory_order_relaxed);
        if (count > g_remotePlayerListMax.load(std::memory_order_relaxed))
            g_remotePlayerListMax.store(count, std::memory_order_relaxed);
        g_localPlayerId.store(localId, std::memory_order_relaxed);
        if (count == 0 || count > SESSION_PLAYER_MAX || !IsLikelyRuntimePointer(entries))
            return;

        std::vector<std::uint8_t> blob(static_cast<std::size_t>(count) * SESSION_PLAYER_STRIDE);
        if (!SafeRead(entries, blob.data(), blob.size()))
            return;

        const DWORD now = GetTickCount();
        std::vector<RemotePlayer> seen;
        std::vector<PlayerWaypointMark> waypoints;
        std::string rawSample;
        for (std::uint64_t index = 0; index < count; ++index)
        {
            const std::uint8_t* entry = blob.data() + index * SESSION_PLAYER_STRIDE;
            if (g_debugLoggingEnabled.load() && index < 6)
            {
                std::ostringstream raw;
                raw << " | p" << index << "=";
                for (std::size_t b = 0; b < 0x28; b += 8)
                {
                    std::uint64_t word = 0;
                    std::memcpy(&word, entry + b, sizeof(word));
                    raw << (b ? " " : "") << std::hex << word << std::dec;
                }
                raw << " hidden=" << static_cast<int>(entry[layout.hiddenOffset]);
                rawSample += raw.str();
            }

            if (entry[SESSION_PLAYER_WAYPOINT_FLAG_OFFSET] != 0 && g_debugLoggingEnabled.load() && index < 4)
            {
                std::ostringstream raw;
                raw << " | wp" << index << "=";
                for (std::size_t b = 0x28; b < 0x78; b += 8)
                {
                    std::uint64_t word = 0;
                    std::memcpy(&word, entry + b, sizeof(word));
                    raw << (b == 0x28 ? "" : " ") << std::hex << word << std::dec;
                }
                rawSample += raw.str();
            }

            if (entry[SESSION_PLAYER_WAYPOINT_FLAG_OFFSET] != 0)
            {
                std::int64_t fixedPosition[3] = {};
                std::memcpy(fixedPosition, entry + SESSION_PLAYER_WAYPOINT_OFFSET, sizeof(fixedPosition));
                PlayerWaypointMark mark{};
                mark.x = FixedToWorld(fixedPosition[0]);
                mark.z = FixedToWorld(fixedPosition[2]);
                mark.lastSeenTick = now;
                if (IsOnMapWorldPosition(mark.x, FixedToWorld(fixedPosition[1]), mark.z))
                    waypoints.push_back(mark);
            }

            if (entry[layout.hiddenOffset] != 0)
                continue;

            RemotePlayer player{};
            std::memcpy(&player.id, entry, sizeof(player.id));
            if (player.id == localId)
            {
                std::string ownName = ReadPlayerNameAt(entry, layout.nameOffset);
                if (!ownName.empty())
                {
                    std::lock_guard<std::mutex> nameLock(g_localPlayerNameMutex);
                    g_localPlayerName = std::move(ownName);
                }
                continue;
            }
            float position[3] = {};
            std::memcpy(position, entry + layout.positionOffset, sizeof(position));
            player.x = position[0];
            player.y = position[1];
            player.z = position[2];
            if (!std::isfinite(player.x) || !std::isfinite(player.z) ||
                !(player.x > 1.0f && player.x < REAL_MAP_WORLD_SIZE + 64.0f && player.z > 1.0f && player.z < REAL_MAP_WORLD_SIZE + 64.0f))
            {
                continue;
            }
            std::memcpy(&player.level, entry + SESSION_PLAYER_LEVEL_OFFSET, sizeof(player.level));
            if (player.level > 999)
                player.level = 0;
            player.name = ReadPlayerNameAt(entry, layout.nameOffset);
            player.lastSeenTick = now;
            seen.push_back(std::move(player));
        }

        // Ping events: the queue is refilled and consumed within a frame, so the slots are
        // read past the count and a slot whose bytes changed counts as a fresh ping.
        {
            std::uint8_t* pingEntries = nullptr;
            std::uint64_t pingCount = 0;
            std::uint64_t pingCapacity = 0;
            if (SafeReadValue(session + UI_PING_EVENT_ARRAY, pingEntries) &&
                SafeReadValue(session + UI_PING_EVENT_ARRAY + 0x08, pingCount) &&
                SafeReadValue(session + UI_PING_EVENT_ARRAY + 0x10, pingCapacity) &&
                pingCapacity != 0 && pingCapacity <= 4096 &&
                IsLikelyRuntimePointer(reinterpret_cast<uintptr_t>(pingEntries)))
            {
                const std::uint64_t slots = MinValue<std::uint64_t>(pingCapacity, PING_EVENT_SLOTS);
                std::vector<std::uint8_t> pingBlob(static_cast<std::size_t>(slots) * UI_PING_EVENT_STRIDE);
                if (SafeRead(reinterpret_cast<uintptr_t>(pingEntries), pingBlob.data(), pingBlob.size()))
                {
                    std::lock_guard<std::mutex> pingLock(g_pingEventMutex);
                    std::ostringstream sample;
                    for (std::uint64_t index = 0; index < slots; ++index)
                    {
                        const std::uint8_t* entry = pingBlob.data() + index * UI_PING_EVENT_STRIDE;
                        const std::uint64_t hash = HashBytes(entry, UI_PING_EVENT_STRIDE);
                        const bool changed = hash != g_pingEventHashes[static_cast<std::size_t>(index)];
                        g_pingEventHashes[static_cast<std::size_t>(index)] = hash;

                        std::int64_t fixedPosition[3] = {};
                        std::memcpy(fixedPosition, entry + UI_PING_EVENT_POSITION_OFFSET, sizeof(fixedPosition));
                        const float pingX = FixedToWorld(fixedPosition[0]);
                        const float pingY = FixedToWorld(fixedPosition[1]);
                        const float pingZ = FixedToWorld(fixedPosition[2]);
                        const bool onMap = IsOnMapWorldPosition(pingX, pingY, pingZ);
                        if ((changed || index < pingCount) && g_debugLoggingEnabled.load() && index < 3)
                        {
                            sample << " | ev" << index << (index < pingCount ? "*" : "") << "=";
                            for (std::size_t b = 0; b < UI_PING_EVENT_STRIDE; b += 8)
                            {
                                std::uint64_t word = 0;
                                std::memcpy(&word, entry + b, sizeof(word));
                                sample << (b ? " " : "") << std::hex << word << std::dec;
                            }
                            sample << " xz=(" << static_cast<int>(pingX) << "," << static_cast<int>(pingZ) << ")";
                        }
                        if (!changed || !onMap)
                            continue;

                        g_pingEventCount.fetch_add(1, std::memory_order_relaxed);
                        // One ping per player: it stays until that player pings elsewhere.
                        std::uint32_t pingPlayer = 0;
                        std::memcpy(&pingPlayer, entry + UI_PING_EVENT_PLAYER_OFFSET, sizeof(pingPlayer));
                        bool merged = false;
                        for (PingEventMark& ping : g_pingEvents)
                        {
                            if (ping.playerId == pingPlayer)
                            {
                                ping.x = pingX;
                                ping.z = pingZ;
                                ping.tick = now;
                                merged = true;
                                break;
                            }
                        }
                        if (!merged && g_pingEvents.size() < PING_EVENT_MAX)
                            g_pingEvents.push_back({ pingX, pingZ, pingPlayer, now });
                    }
                    if (!sample.str().empty())
                        g_pingEventSample = sample.str();
                }
            }
        }

        std::lock_guard<std::mutex> lock(g_remotePlayerMutex);
        if (!rawSample.empty())
            g_remotePlayerRawSample = rawSample;
        if (!waypoints.empty() || count != 0)
            g_playerWaypoints = std::move(waypoints);
        for (RemotePlayer& player : seen)
        {
            bool updated = false;
            for (RemotePlayer& known : g_remotePlayers)
            {
                if (known.id == player.id)
                {
                    if (player.name.empty())
                        player.name = known.name;
                    known = std::move(player);
                    updated = true;
                    break;
                }
            }
            if (!updated)
                g_remotePlayers.push_back(std::move(player));
        }
        g_remotePlayers.erase(
            std::remove_if(g_remotePlayers.begin(), g_remotePlayers.end(), [now](const RemotePlayer& player) {
                return TicksSince(now, player.lastSeenTick) > REMOTE_PLAYER_HOLD_MS;
            }),
            g_remotePlayers.end());
    }

    std::vector<PlayerWaypointMark> CopyPlayerWaypoints()
    {
        const DWORD now = GetTickCount();
        std::lock_guard<std::mutex> lock(g_remotePlayerMutex);
        std::vector<PlayerWaypointMark> result;
        for (const PlayerWaypointMark& mark : g_playerWaypoints)
        {
            if (TicksSince(now, mark.lastSeenTick) <= REMOTE_PLAYER_HOLD_MS)
                result.push_back(mark);
        }
        return result;
    }

    std::string ResolvePlayerName(std::uint32_t playerId)
    {
        if (playerId != 0 && playerId == g_localPlayerId.load())
        {
            std::lock_guard<std::mutex> lock(g_localPlayerNameMutex);
            return g_localPlayerName;
        }
        std::lock_guard<std::mutex> lock(g_remotePlayerMutex);
        for (const RemotePlayer& player : g_remotePlayers)
        {
            if (player.id == playerId)
                return player.name;
        }
        return std::string();
    }

    std::vector<RemotePlayer> CopyRemotePlayers()
    {
        const DWORD now = GetTickCount();
        std::lock_guard<std::mutex> lock(g_remotePlayerMutex);
        std::vector<RemotePlayer> result;
        for (const RemotePlayer& player : g_remotePlayers)
        {
            if (TicksSince(now, player.lastSeenTick) <= REMOTE_PLAYER_HOLD_MS)
                result.push_back(player);
        }
        return result;
    }

    void LogRemotePlayersIfDue()
    {
        if (!g_debugLoggingEnabled.load())
            return;
        static std::atomic<DWORD> lastLogTick{ 0 };
        const DWORD now = GetTickCount();
        const DWORD last = lastLogTick.load();
        if (last != 0 && TicksSince(now, last) < 10000)
            return;
        lastLogTick.store(now);

        const std::vector<RemotePlayer> players = CopyRemotePlayers();
        std::string rawSample;
        {
            std::lock_guard<std::mutex> lock(g_remotePlayerMutex);
            rawSample = g_remotePlayerRawSample;
        }
        std::ostringstream oss;
        const std::vector<PlayerWaypointMark> marks = CopyPlayerWaypoints();
        oss << "[Minimap] remote players | shown=" << players.size()
            << " pings=" << marks.size()
            << " | list=" << g_remotePlayerListSize.load()
            << " max_list=" << g_remotePlayerListMax.load()
            << " local_id=" << g_localPlayerId.load()
            << " reads=" << g_remotePlayerReads.load()
            << " layout=" << g_sessionLayoutState.load();
        for (const RemotePlayer& player : players)
        {
            oss << " | " << (player.name.empty() ? std::string("?") : player.name)
                << " Lv" << player.level
                << " #" << player.id
                << " (" << static_cast<int>(player.x) << "," << static_cast<int>(player.y) << "," << static_cast<int>(player.z) << ")";
        }
        for (const PlayerWaypointMark& mark : marks)
            oss << " | ping(" << static_cast<int>(mark.x) << "," << static_cast<int>(mark.z) << ")";
        oss << " | live_pings=" << CopyPingEvents().size()
            << " | drawn=" << g_pingPointsDrawn.load() << "/" << g_pingPointsBuilt.load()
            << " sprite_ok=" << g_pingSpriteDrawn.load()
            << " | ping_events=" << g_pingEventCount.load();
        {
            std::lock_guard<std::mutex> pingLock(g_pingEventMutex);
            oss << g_pingEventSample;
        }
        oss << rawSample;
        Log(oss.str());
    }
    // ---- END REMOTE PLAYERS ----


    void BuildWorldPoints(
        std::vector<MinimapWorldPoint>& points,
        const std::vector<CapturedWaypoint>& waypoints,
        const std::vector<CapturedNearbyMarker>& nearbyMarkers,
        const std::vector<CapturedMapMarkerVisibility>& visibleMapMarkers)
    {
        std::vector<MinimapWorldPoint> visibleMarkerPoints;
        visibleMarkerPoints.reserve(visibleMapMarkers.size());
        for (const CapturedMapMarkerVisibility& marker : visibleMapMarkers)
        {
            if (!IsVisibleMapMarkerDrawable(marker))
                continue;

            visibleMarkerPoints.push_back({ FixedToWorld(marker.x), FixedToWorld(marker.z), ResolveCapturedVisibleMapMarkerKind(marker) });
        }

        // Live pings (they only exist for a few seconds), with the name of whoever set them.
        for (const PingEventMark& ping : CopyPingEvents())
            points.push_back({ ping.x, ping.z, MAP_MARKER_KEY_PLAYER_PING, EnsureNameSprite(ResolvePlayerName(ping.playerId)) });

        // Other players first so nearby map icons cannot swallow them.
        for (const RemotePlayer& player : CopyRemotePlayers())
        {
            points.push_back({ player.x, player.z, 13, EnsureNameSprite(player.name) });
        }

        for (const CapturedWaypoint& waypoint : waypoints)
        {
            PushWorldPointUnique(points, { FixedToWorld(waypoint.x), FixedToWorld(waypoint.z), ResolveCapturedWaypointKind(waypoint) });
        }

        // Master world-map markers: exactly what the big map shows. Atlas keys draw the
        // native raster icons; special keys map to dedicated vector icons.
        {
            std::lock_guard<std::mutex> lock(g_masterMarkerMutex);
            for (const CapturedMasterMarker& marker : g_masterMarkers)
            {
                std::uint32_t kind;
                std::uint32_t knownKey = 0;
                if (marker.source != 0)
                {
                    // The other two world-map arrays hold map markers of other categories
                    // (replicate_map_markers_for_ui, exe 0x1402a9c50 picks the array by
                    // marker category), not players: draw only real icons.
                    if (!TryResolveKnownMarkerIconKey(marker.key, knownKey))
                        continue;
                    kind = marker.key;
                }
                else if (marker.key == 0)
                    kind = 12;      // NPC
                else if (TryResolveKnownMarkerIconKey(marker.key, knownKey))
                    kind = marker.key;
                else if (marker.moving)
                    kind = 13;      // remote player: a moving marker without a map icon
                else if (IsIconlessMapMarkerKey(marker.key))
                    kind = 12;      // registry types without an icon: NPC-style markers (portrait drawn by the game)
                else if (marker.key == MASTER_KEY_FLAME_ALTAR)
                    kind = 31;
                else
                    kind = 55;

                // NPCs stand a couple of metres apart at a base; the 4 m de-duplication
                // meant for stacked POI icons swallowed most of them.
                if (kind == 12)
                    points.push_back({ FixedToWorld(marker.x), FixedToWorld(marker.z), kind });
                else
                    PushWorldPointUnique(points, { FixedToWorld(marker.x), FixedToWorld(marker.z), kind });
            }
        }

        for (const CapturedNearbyMarker& marker : nearbyMarkers)
        {
            if (!marker.hasWorldPosition)
                continue;

            const std::uint32_t resolvedKind = ResolveCapturedMarkerKind(marker);
            if (!IsRuntimeMarkerKind(marker.kind) && resolvedKind <= 0xFFFFu)
                continue;

            if (!visibleMarkerPoints.empty() &&
                !IsPersonalMarkerKind(marker.kind) &&
                !HasVisibleMapMarkerNear(visibleMarkerPoints, FixedToWorld(marker.x), FixedToWorld(marker.z), resolvedKind))
            {
                continue;
            }

            PushWorldPointUnique(points, { FixedToWorld(marker.x), FixedToWorld(marker.z), resolvedKind });
        }

        for (const MinimapWorldPoint& marker : visibleMarkerPoints)
        {
            PushWorldPointUnique(points, marker);
        }

        // Each player's active waypoint highlights the icon standing on that spot with a
        // yellow outline of that icon's own shape, exactly like the world map does; with
        // no icon there, a plain yellow ring marks the place.
        for (const PlayerWaypointMark& mark : CopyPlayerWaypoints())
        {
            g_pingPointsBuilt.fetch_add(1, std::memory_order_relaxed);
            MinimapWorldPoint* nearest = nullptr;
            float nearestDistance = 8.0f * 8.0f;
            for (MinimapWorldPoint& point : points)
            {
                if (point.kind <= 0xFFFFu)
                    continue;
                const float dx = point.x - mark.x;
                const float dz = point.z - mark.z;
                const float distance = dx * dx + dz * dz;
                if (distance < nearestDistance)
                {
                    nearestDistance = distance;
                    nearest = &point;
                }
            }

            const std::uint32_t silhouette = nearest != nullptr ? FindIconSilhouetteKey(nearest->kind) : 0;
            if (silhouette != 0)
                nearest->highlight = silhouette;
            else
                points.push_back({ mark.x, mark.z, MAP_MARKER_KEY_WAYPOINT, 0, 0 });   // the icon carries its own outline
        }
    }

    bool HasVisibleRuntimeMarkerNear(const std::vector<MinimapWorldPoint>& genericRuntimeMarkers, float x, float z)
    {
        constexpr float kMatchRadiusWorld = 72.0f;
        constexpr float kMatchRadiusSq = kMatchRadiusWorld * kMatchRadiusWorld;
        for (const MinimapWorldPoint& marker : genericRuntimeMarkers)
        {
            const float dx = marker.x - x;
            const float dz = marker.z - z;
            if (dx * dx + dz * dz <= kMatchRadiusSq)
                return true;
        }

        return false;
    }

    bool HasRuntimeMarkersNearCenter(const std::vector<MinimapWorldPoint>& genericRuntimeMarkers, float centerX, float centerZ, float radiusWorld)
    {
        const float usableRadius = radiusWorld + 96.0f;
        const float usableRadiusSq = usableRadius * usableRadius;
        for (const MinimapWorldPoint& marker : genericRuntimeMarkers)
        {
            const float dx = marker.x - centerX;
            const float dz = marker.z - centerZ;
            if (dx * dx + dz * dz <= usableRadiusSq)
                return true;
        }

        return false;
    }

    bool HasWorldPointsNearCenter(const std::vector<MinimapWorldPoint>& markers, float centerX, float centerZ, float radiusWorld)
    {
        const float usableRadius = radiusWorld + 96.0f;
        const float usableRadiusSq = usableRadius * usableRadius;
        for (const MinimapWorldPoint& marker : markers)
        {
            const float dx = marker.x - centerX;
            const float dz = marker.z - centerZ;
            if (dx * dx + dz * dz <= usableRadiusSq)
                return true;
        }

        return false;
    }

    bool HasVisibleMapMarkerNear(const std::vector<MinimapWorldPoint>& visibleMarkers, float x, float z, std::uint32_t kind)
    {
        constexpr float kMatchRadiusWorld = 56.0f;
        constexpr float kMatchRadiusSq = kMatchRadiusWorld * kMatchRadiusWorld;
        for (const MinimapWorldPoint& marker : visibleMarkers)
        {
            const float dx = marker.x - x;
            const float dz = marker.z - z;
            if (dx * dx + dz * dz > kMatchRadiusSq)
                continue;

            if (kind > 0xFFFFu)
            {
                if (marker.kind == kind)
                    return true;
                continue;
            }

            return true;
        }

        return false;
    }

    void AppendVisibleStaticPoiPoints(
        std::vector<MinimapWorldPoint>& points,
        const std::vector<CapturedNearbyMarker>& nearbyMarkers,
        const std::vector<CapturedMapMarkerVisibility>& visibleMapMarkers,
        float centerX,
        float centerZ,
        float radiusWorld)
    {
        EnsureStaticPoisLoaded();

        std::vector<MinimapWorldPoint> genericRuntimeMarkers;
        genericRuntimeMarkers.reserve(nearbyMarkers.size());
        for (const CapturedNearbyMarker& marker : nearbyMarkers)
        {
            if (!marker.hasWorldPosition || !IsRuntimeMarkerKind(marker.kind))
                continue;

            const std::uint32_t resolvedKind = ResolveCapturedMarkerKind(marker);
            if (resolvedKind > 0xFFFFu)
                continue;

            genericRuntimeMarkers.push_back({ FixedToWorld(marker.x), FixedToWorld(marker.z), resolvedKind });
        }

        std::vector<MinimapWorldPoint> visibleMarkerPoints;
        visibleMarkerPoints.reserve(visibleMapMarkers.size());
        for (const CapturedMapMarkerVisibility& marker : visibleMapMarkers)
        {
            if (!IsVisibleMapMarkerDrawable(marker))
                continue;

            visibleMarkerPoints.push_back({ FixedToWorld(marker.x), FixedToWorld(marker.z), ResolveCapturedVisibleMapMarkerKind(marker) });
        }

        const bool requireRuntimeMatch = HasRuntimeMarkersNearCenter(genericRuntimeMarkers, centerX, centerZ, radiusWorld);
        const bool requireVisibilityMatch = HasWorldPointsNearCenter(visibleMarkerPoints, centerX, centerZ, radiusWorld);

        struct StaticPoiCandidate
        {
            MinimapWorldPoint point;
            float distanceSq = 0.0f;
        };

        std::vector<StaticPoiCandidate> candidates;
        {
            std::lock_guard<std::mutex> lock(g_staticPoiMutex);
            if (!g_staticPois.loaded)
                return;

            const float radiusSq = radiusWorld * radiusWorld;
            candidates.reserve(MinValue(g_staticPois.pois.size(), STATIC_POI_MAX_DRAWN));
            const auto hasPointNear = [&points](float x, float z)
            {
                for (const MinimapWorldPoint& existing : points)
                {
                    const float dx = existing.x - x;
                    const float dz = existing.z - z;
                    if (dx * dx + dz * dz < 25.0f * 25.0f)
                        return true;
                }
                return false;
            };

            for (const StaticPoi& poi : g_staticPois.pois)
            {
                // Distance first: the fog lookup takes a mutex, and only the handful of
                // POIs inside the minimap need it (not all 567, every frame).
                const float dx = poi.x - centerX;
                const float dz = poi.z - centerZ;
                const float distanceSq = dx * dx + dz * dz;
                if (distanceSq > radiusSq)
                    continue;

                if (!IsWorldPointRevealedByFogOfWar(poi.x, poi.z))
                    continue;

                // The live master-marker feed already covers this spot with the real
                // icon; skip the static catalog duplicate.
                if (hasPointNear(poi.x, poi.z))
                    continue;

                if (requireVisibilityMatch && !HasVisibleMapMarkerNear(visibleMarkerPoints, poi.x, poi.z, poi.kind))
                    continue;

                if (requireRuntimeMatch && !HasVisibleRuntimeMarkerNear(genericRuntimeMarkers, poi.x, poi.z))
                    continue;

                candidates.push_back({ { poi.x, poi.z, poi.kind }, distanceSq });
            }
        }

        std::sort(candidates.begin(), candidates.end(), [](const StaticPoiCandidate& left, const StaticPoiCandidate& right)
        {
            return left.distanceSq < right.distanceSq;
        });

        const std::size_t count = MinValue(candidates.size(), STATIC_POI_MAX_DRAWN);
        for (std::size_t index = 0; index < count; ++index)
        {
            points.push_back(candidates[index].point);
        }
    }

    bool ComputeMapCenter(const std::vector<MinimapWorldPoint>& points, float& centerX, float& centerZ)
    {
        if (points.empty())
            return false;

        if (points.size() == 1)
        {
            centerX = points[0].x - 260.0f;
            centerZ = points[0].z + 170.0f;
            return true;
        }

        float sumX = 0.0f;
        float sumZ = 0.0f;
        for (const MinimapWorldPoint& point : points)
        {
            sumX += point.x;
            sumZ += point.z;
        }

        centerX = sumX / static_cast<float>(points.size());
        centerZ = sumZ / static_cast<float>(points.size());
        return true;
    }

    void LimitMinimapWorldPoints(std::vector<MinimapWorldPoint>& points, float centerX, float centerZ)
    {
        const int maxPoints = ClampValue(g_minimapMaxDrawnPoints.load(), 8, 128);
        if (points.size() <= static_cast<std::size_t>(maxPoints))
            return;

        std::stable_sort(points.begin(), points.end(), [centerX, centerZ](const MinimapWorldPoint& left, const MinimapWorldPoint& right)
        {
            // Players, pings and waypoints survive the cap even when they are far away.
            const auto priority = [](const MinimapWorldPoint& point)
            {
                return point.kind == 13 || point.kind == MAP_MARKER_KEY_PLAYER_PING ||
                    point.kind == MAP_MARKER_KEY_WAYPOINT || point.highlight != 0;
            };
            const bool leftPlayer = priority(left);
            const bool rightPlayer = priority(right);
            if (leftPlayer != rightPlayer)
                return leftPlayer;
            const float leftDx = left.x - centerX;
            const float leftDz = left.z - centerZ;
            const float rightDx = right.x - centerX;
            const float rightDz = right.z - centerZ;
            return (leftDx * leftDx + leftDz * leftDz) < (rightDx * rightDx + rightDz * rightDz);
        });
        points.resize(static_cast<std::size_t>(maxPoints));
    }

    bool ProjectWorldToMinimap(float worldX, float worldZ, float centerX, float centerZ, float unitsPerPixel, float headingRadians, int cx, int cy, int radius, int& outX, int& outY, bool& clipped)
    {
        const float dx = (worldX - centerX) / unitsPerPixel;
        const float dz = (worldZ - centerZ) / unitsPerPixel;
        const float cosHeading = std::cos(headingRadians);
        const float sinHeading = std::sin(headingRadians);
        const float northUpX = dx;
        const float northUpY = -dz;
        float screenX = cosHeading * northUpX + sinHeading * northUpY;
        float screenY = -sinHeading * northUpX + cosHeading * northUpY;
        // Square window: clamp along the ray so edge markers keep their direction.
        const float limit = static_cast<float>(MaxValue(1, radius - 18));
        const float distance = MaxValue(std::fabs(screenX), std::fabs(screenY));
        clipped = distance > limit;
        if (clipped && distance > 0.01f)
        {
            const float scale = limit / distance;
            screenX *= scale;
            screenY *= scale;
        }

        outX = cx + static_cast<int>(screenX);
        outY = cy + static_cast<int>(screenY);
        return true;
    }

    void DrawRealMap(VulkanMinimapRenderer& renderer, void* commandBuffer, int cx, int cy, int radius, float centerX, float centerZ, float unitsPerPixel, float headingRadians)
    {
        EnsureRealMapLoaded();

        const int innerRadius = MaxValue(8, radius);
        const float cosHeading = std::cos(headingRadians);
        const float sinHeading = std::sin(headingRadians);
        std::lock_guard<std::mutex> lock(g_realMapMutex);
        const bool hasMap = g_realMap.loaded && !g_realMap.rgba.empty();
        const int sampleStep = ClampValue(g_minimapMapSampleStep.load(), 1, 4);
        const float lightLift = static_cast<float>(ClampValue(g_minimapMapLight.load(), 0, 100)) / 100.0f * 0.62f;
        const auto quantizeColor = [](float value) -> float
        {
            // 34 steps caused visible color banding; 64 hurt FPS (shorter runs, more
            // clear-rects). 48 is the measured compromise.
            constexpr float kSteps = 48.0f;
            return std::round(ClampValue(value, 0.0f, 1.0f) * kSteps) / kSteps;
        };

        for (int y = -innerRadius; y <= innerRadius; y += sampleStep)
        {
            const int rowHeight = MinValue(sampleStep, innerRadius - y + 1);
            const int sampleY = y + rowHeight / 2;
            int runStart = 0;
            float runRed = 0.0f;
            float runGreen = 0.0f;
            float runBlue = 0.0f;
            bool inRun = false;

            const auto flushRun = [&](int endX)
            {
                if (!inRun || endX <= runStart)
                    return;

                CmdClearRect(renderer, commandBuffer, runRed, runGreen, runBlue, 1.0f, cx + runStart, cy + y, endX - runStart, rowHeight);
                inRun = false;
            };

            for (int x = -innerRadius; x <= innerRadius; x += sampleStep)
            {
                const int columnWidth = MinValue(sampleStep, innerRadius - x + 1);
                const int sampleX = x + columnWidth / 2;
                if (!IsInsideMinimapWindow(sampleX, sampleY, innerRadius))
                {
                    flushRun(x);
                    continue;
                }

                float red = 0.045f;
                float green = 0.048f;
                float blue = 0.042f;
                if (hasMap)
                {
                    const float northUpX = cosHeading * static_cast<float>(sampleX) - sinHeading * static_cast<float>(sampleY);
                    const float northUpY = sinHeading * static_cast<float>(sampleX) + cosHeading * static_cast<float>(sampleY);
                    const float worldX = centerX + northUpX * unitsPerPixel;
                    const float worldZ = centerZ - northUpY * unitsPerPixel;
                    const float u = ClampValue(worldX / REAL_MAP_WORLD_SIZE, 0.0f, 1.0f);
                    const float v = ClampValue(1.0f - (worldZ / REAL_MAP_WORLD_SIZE), 0.0f, 1.0f);
                    const float mapFx = u * static_cast<float>(g_realMap.width - 1);
                    const float mapFy = v * static_cast<float>(g_realMap.height - 1);
                    const int x0 = ClampValue(static_cast<int>(std::floor(mapFx)), 0, g_realMap.width - 1);
                    const int y0 = ClampValue(static_cast<int>(std::floor(mapFy)), 0, g_realMap.height - 1);
                    const int x1 = ClampValue(x0 + 1, 0, g_realMap.width - 1);
                    const int y1 = ClampValue(y0 + 1, 0, g_realMap.height - 1);
                    const float tx = mapFx - static_cast<float>(x0);
                    const float ty = mapFy - static_cast<float>(y0);

                    const auto sampleChannel = [&](int x, int y, int channel) -> float
                    {
                        const std::size_t offset = (static_cast<std::size_t>(y) * static_cast<std::size_t>(g_realMap.width) + static_cast<std::size_t>(x)) * 4;
                        return static_cast<float>(g_realMap.rgba[offset + static_cast<std::size_t>(channel)]) / 255.0f;
                    };

                    const float redTop = sampleChannel(x0, y0, 0) * (1.0f - tx) + sampleChannel(x1, y0, 0) * tx;
                    const float redBottom = sampleChannel(x0, y1, 0) * (1.0f - tx) + sampleChannel(x1, y1, 0) * tx;
                    const float greenTop = sampleChannel(x0, y0, 1) * (1.0f - tx) + sampleChannel(x1, y0, 1) * tx;
                    const float greenBottom = sampleChannel(x0, y1, 1) * (1.0f - tx) + sampleChannel(x1, y1, 1) * tx;
                    const float blueTop = sampleChannel(x0, y0, 2) * (1.0f - tx) + sampleChannel(x1, y0, 2) * tx;
                    const float blueBottom = sampleChannel(x0, y1, 2) * (1.0f - tx) + sampleChannel(x1, y1, 2) * tx;

                    red = redTop * (1.0f - ty) + redBottom * ty;
                    green = greenTop * (1.0f - ty) + greenBottom * ty;
                    blue = blueTop * (1.0f - ty) + blueBottom * ty;

                    // Shroud overlay (same math as the GPU map shader).
                    const float alphaTop = sampleChannel(x0, y0, 3) * (1.0f - tx) + sampleChannel(x1, y0, 3) * tx;
                    const float alphaBottom = sampleChannel(x0, y1, 3) * (1.0f - tx) + sampleChannel(x1, y1, 3) * tx;
                    const float shroudDistance = (alphaTop * (1.0f - ty) + alphaBottom * ty - 0.50196f) * (255.0f * SHROUD_SDF_RANGE / 127.0f);
                    if (shroudDistance < 2.0f * unitsPerPixel)
                    {
                        const float pixelWorld = MaxValue(unitsPerPixel, 0.01f);
                        const float shroudCover = ClampValue(0.5f - shroudDistance / (1.4f * pixelWorld), 0.0f, 1.0f);
                        const float edge = ClampValue((1.6f * pixelWorld - std::fabs(shroudDistance)) / pixelWorld, 0.0f, 1.0f) * 0.85f;
                        red += (red * 0.60f + 0.17f - red) * shroudCover;
                        green += (green * 0.60f + 0.27f - green) * shroudCover;
                        blue += (blue * 0.60f + 0.36f - blue) * shroudCover;
                        red += (0.62f - red) * edge;
                        green += (0.82f - green) * edge;
                        blue += (0.96f - blue) * edge;
                    }

                    red = ClampValue((red - 0.42f) * 1.20f + 0.42f, 0.0f, 1.0f);
                    green = ClampValue((green - 0.42f) * 1.22f + 0.42f, 0.0f, 1.0f);
                    blue = ClampValue((blue - 0.42f) * 1.16f + 0.42f, 0.0f, 1.0f);

                    // Parchment lift toward the big map's light look: brighten toward
                    // paper-white, then warm the tint (blue drops the most).
                    red = red + (1.0f - red) * lightLift;
                    green = (green + (1.0f - green) * lightLift) * 0.965f;
                    blue = (blue + (1.0f - blue) * lightLift) * 0.885f;

                    const float distance = static_cast<float>(MinimapWindowDistance(sampleX, sampleY)) / static_cast<float>(innerRadius);
                    const float vignette = 1.01f - 0.10f * distance * distance;
                    red = ClampValue(red * vignette + 0.004f, 0.0f, 1.0f);
                    green = ClampValue(green * vignette + 0.004f, 0.0f, 1.0f);
                    blue = ClampValue(blue * vignette + 0.004f, 0.0f, 1.0f);
                }

                red = quantizeColor(red);
                green = quantizeColor(green);
                blue = quantizeColor(blue);

                if (inRun &&
                    std::fabs(runRed - red) < 0.0001f &&
                    std::fabs(runGreen - green) < 0.0001f &&
                    std::fabs(runBlue - blue) < 0.0001f)
                {
                    continue;
                }

                flushRun(x);
                runStart = x;
                runRed = red;
                runGreen = green;
                runBlue = blue;
                inRun = true;
            }

            flushRun(innerRadius + 1);
        }
    }

    void CmdClearFreeLine(VulkanMinimapRenderer& renderer, void* commandBuffer, int x0, int y0, int x1, int y1, int size, float red, float green, float blue)
    {
        const int steps = MaxValue(1, MaxValue(std::abs(x1 - x0), std::abs(y1 - y0)));
        const int half = MaxValue(1, size / 2);
        std::vector<VkClearRect> rects;
        rects.reserve(static_cast<std::size_t>(steps / MaxValue(1, half) + 2));
        for (int step = 0; step <= steps; step += MaxValue(1, half))
        {
            const float t = static_cast<float>(step) / static_cast<float>(steps);
            const int x = static_cast<int>(static_cast<float>(x0) + static_cast<float>(x1 - x0) * t);
            const int y = static_cast<int>(static_cast<float>(y0) + static_cast<float>(y1 - y0) * t);
            AppendClippedClearRect(renderer, rects, x - half, y - half, half * 2 + 1, half * 2 + 1);
        }
        CmdClearRects(renderer, commandBuffer, red, green, blue, 1.0f, rects);
    }

    int MinimapRasterFrameExtra(int radius)
    {
        return ClampValue((radius * 45) / 100, 46, 64);
    }

    int MinimapRasterMapRadius(int radius, int frameExtra)
    {
        return MaxValue(radius, (((radius + frameExtra) * 188) + 255) / 256 + 3);
    }

    bool TryDrawMinimapRasterFrame(VulkanMinimapRenderer& renderer, void* commandBuffer, int cx, int cy, int radius, int frameExtra)
    {
        EnsureMinimapFrameLoaded();

        std::lock_guard<std::mutex> lock(g_minimapFrameMutex);
        if (!g_minimapFrame.loaded || g_minimapFrame.width <= 0 || g_minimapFrame.height <= 0)
            return false;

        const int targetSize = (radius + frameExtra) * 2;
        const int visualCenterYOffset = -MaxValue(1, (targetSize + 128) / 256);
        const int left = cx - targetSize / 2;
        const int top = cy - targetSize / 2 + visualCenterYOffset;
        constexpr std::size_t kChunkSize = 1024;

        for (const MinimapFrameColorGroup& group : g_minimapFrame.groups)
        {
            std::vector<VkClearRect> rects;
            rects.reserve(kChunkSize);

            const auto flush = [&]()
            {
                if (rects.empty())
                    return;

                CmdClearRects(
                    renderer,
                    commandBuffer,
                    static_cast<float>(group.red) / 255.0f,
                    static_cast<float>(group.green) / 255.0f,
                    static_cast<float>(group.blue) / 255.0f,
                    1.0f,
                    rects);
                rects.clear();
            };

            for (const MinimapFrameRun& run : group.runs)
            {
                const int x0 = left + (static_cast<int>(run.x) * targetSize) / g_minimapFrame.width;
                const int x1 = left + ((static_cast<int>(run.x) + static_cast<int>(run.width)) * targetSize + g_minimapFrame.width - 1) / g_minimapFrame.width;
                const int y0 = top + (static_cast<int>(run.y) * targetSize) / g_minimapFrame.height;
                const int y1 = top + ((static_cast<int>(run.y) + 1) * targetSize + g_minimapFrame.height - 1) / g_minimapFrame.height;
                AppendClippedClearRect(renderer, rects, x0, y0, MaxValue(1, x1 - x0), MaxValue(1, y1 - y0));

                if (rects.size() >= kChunkSize)
                    flush();
            }

            flush();
        }

        return true;
    }

    void RecordFrameTextureUploadIfNeeded(VulkanMinimapRenderer& renderer, void* commandBuffer)
    {
        if (!renderer.frameTextureReady || !renderer.frameTextureUploadPending)
            return;

        void* image = reinterpret_cast<void*>(renderer.frameTextureImage);
        void* buffer = reinterpret_cast<void*>(renderer.frameStagingBuffer);
        if (image == nullptr || buffer == nullptr)
            return;

        VkImageMemoryBarrier toTransfer{};
        toTransfer.srcAccessMask = 0;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.image = image;
        toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toTransfer.subresourceRange.levelCount = 1;
        toTransfer.subresourceRange.layerCount = 1;
        renderer.fns.cmdPipelineBarrier(
            commandBuffer,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,
            0,
            nullptr,
            0,
            nullptr,
            1,
            &toTransfer);

        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent.width = renderer.frameTextureWidth;
        copy.imageExtent.height = renderer.frameTextureHeight;
        copy.imageExtent.depth = 1;
        renderer.fns.cmdCopyBufferToImage(
            commandBuffer,
            buffer,
            image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1,
            &copy);

        VkImageMemoryBarrier toShader{};
        toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toShader.image = image;
        toShader.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toShader.subresourceRange.levelCount = 1;
        toShader.subresourceRange.layerCount = 1;
        renderer.fns.cmdPipelineBarrier(
            commandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0,
            0,
            nullptr,
            0,
            nullptr,
            1,
            &toShader);

        renderer.frameTextureUploadPending = false;
    }

    bool TryDrawMinimapTexturedFrame(VulkanMinimapRenderer& renderer, void* commandBuffer, int cx, int cy, int radius, int frameExtra, float headingRadians)
    {
        if (!renderer.frameTextureReady || renderer.framePipeline == 0 || renderer.framePipelineLayout == 0 || renderer.frameDescriptorSet == 0)
            return false;

        const int targetSize = (radius + frameExtra) * 2;
        const int visualCenterYOffset = -MaxValue(1, (targetSize + 128) / 256);
        const int left = cx - targetSize / 2;
        const int top = cy - targetSize / 2 + visualCenterYOffset;
        const int right = left + targetSize;
        const int bottom = top + targetSize;

        const float cosHeading = std::cos(headingRadians);
        const float sinHeading = std::sin(headingRadians);
        const float push[8] = {
            (static_cast<float>(left) / static_cast<float>(renderer.width)) * 2.0f - 1.0f,
            (static_cast<float>(top) / static_cast<float>(renderer.height)) * 2.0f - 1.0f,
            (static_cast<float>(right) / static_cast<float>(renderer.width)) * 2.0f - 1.0f,
            (static_cast<float>(bottom) / static_cast<float>(renderer.height)) * 2.0f - 1.0f,
            cosHeading,
            sinHeading,
            static_cast<float>(renderer.height) / static_cast<float>(renderer.width),
            static_cast<float>(renderer.width) / static_cast<float>(renderer.height)
        };

        void* descriptorSet = reinterpret_cast<void*>(renderer.frameDescriptorSet);
        renderer.fns.cmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, reinterpret_cast<void*>(renderer.framePipeline));
        renderer.boundPipeline = renderer.framePipeline;
        renderer.boundDescriptorSet = renderer.frameDescriptorSet;
        renderer.fns.cmdBindDescriptorSets(
            commandBuffer,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            reinterpret_cast<void*>(renderer.framePipelineLayout),
            0,
            1,
            &descriptorSet,
            0,
            nullptr);
        renderer.fns.cmdPushConstants(
            commandBuffer,
            reinterpret_cast<void*>(renderer.framePipelineLayout),
            VK_SHADER_STAGE_VERTEX_BIT,
            0,
            sizeof(push),
            push);
        renderer.fns.cmdDraw(commandBuffer, 6, 1, 0, 0);
        return true;
    }

    void CmdClearCompassLetter(VulkanMinimapRenderer& renderer, void* commandBuffer, char letter, int x, int y, int size, float red, float green, float blue)
    {
        const int w = MaxValue(10, size);
        const int h = MaxValue(14, size + 4);
        const int t = MaxValue(2, size / 6);
        const int left = x - w / 2;
        const int top = y - h / 2;
        const int right = left + w;
        const int bottom = top + h;
        const int midY = y;

        switch (letter)
        {
        case 'N':
            CmdClearRect(renderer, commandBuffer, red, green, blue, 1.0f, left, top, t, h);
            CmdClearRect(renderer, commandBuffer, red, green, blue, 1.0f, right - t, top, t, h);
            CmdClearFreeLine(renderer, commandBuffer, left + t, bottom - 1, right - t, top, t, red, green, blue);
            break;
        case 'E':
            CmdClearRect(renderer, commandBuffer, red, green, blue, 1.0f, left, top, t, h);
            CmdClearRect(renderer, commandBuffer, red, green, blue, 1.0f, left, top, w, t);
            CmdClearRect(renderer, commandBuffer, red, green, blue, 1.0f, left, midY - t / 2, w - 2, t);
            CmdClearRect(renderer, commandBuffer, red, green, blue, 1.0f, left, bottom - t, w, t);
            break;
        case 'S':
            CmdClearRect(renderer, commandBuffer, red, green, blue, 1.0f, left + 1, top, w - 1, t);
            CmdClearRect(renderer, commandBuffer, red, green, blue, 1.0f, left, top, t, h / 2);
            CmdClearRect(renderer, commandBuffer, red, green, blue, 1.0f, left + 1, midY - t / 2, w - 1, t);
            CmdClearRect(renderer, commandBuffer, red, green, blue, 1.0f, right - t, midY, t, h / 2);
            CmdClearRect(renderer, commandBuffer, red, green, blue, 1.0f, left, bottom - t, w - 1, t);
            break;
        case 'W':
            CmdClearFreeLine(renderer, commandBuffer, left, top, left + w / 4, bottom, t, red, green, blue);
            CmdClearFreeLine(renderer, commandBuffer, left + w / 4, bottom, x, top + h / 3, t, red, green, blue);
            CmdClearFreeLine(renderer, commandBuffer, x, top + h / 3, left + (w * 3) / 4, bottom, t, red, green, blue);
            CmdClearFreeLine(renderer, commandBuffer, left + (w * 3) / 4, bottom, right, top, t, red, green, blue);
            break;
        default:
            break;
        }
    }

    void CmdClearCompassBadge(VulkanMinimapRenderer& renderer, void* commandBuffer, int x, int y, char letter)
    {
        CmdClearSolidDiamond(renderer, commandBuffer, x + 2, y + 3, 17, 0.0f, 0.0f, 0.0f, 0.58f);
        CmdClearSolidDiamond(renderer, commandBuffer, x, y, 16, 0.76f, 0.57f, 0.30f);
        CmdClearSolidDiamond(renderer, commandBuffer, x, y, 13, 0.080f, 0.062f, 0.044f);
        CmdClearCircle(renderer, commandBuffer, 0.82f, 0.63f, 0.34f, 1.0f, x, y, 9, 2);
        CmdClearCircle(renderer, commandBuffer, 0.026f, 0.024f, 0.022f, 1.0f, x, y, 7, 2);
        CmdClearCompassLetter(renderer, commandBuffer, letter, x, y + 1, letter == 'W' ? 13 : 12, 0.96f, 0.86f, 0.63f);
    }

    void CmdClearRimJewel(VulkanMinimapRenderer& renderer, void* commandBuffer, int x, int y)
    {
        CmdClearCircle(renderer, commandBuffer, 0.0f, 0.0f, 0.0f, 0.50f, x + 1, y + 1, 7, 2);
        CmdClearSolidDiamond(renderer, commandBuffer, x, y, 6, 0.76f, 0.56f, 0.27f);
        CmdClearSolidDiamond(renderer, commandBuffer, x, y, 4, 0.045f, 0.115f, 0.125f);
        CmdClearSolidDiamond(renderer, commandBuffer, x, y, 3, 0.32f, 0.90f, 0.96f);
        CmdClearSolidDiamond(renderer, commandBuffer, x, y, 1, 0.86f, 1.0f, 1.0f);
    }

    // Fallback frame (used only when embervale_minimap_frame.rgba is missing): square
    // bands with compass badges on the edge midpoints and jewels on the corners.
    void CmdClearSquareBand(VulkanMinimapRenderer& renderer, void* commandBuffer, float red, float green, float blue, float alpha, int cx, int cy, int outerHalf, int innerHalf)
    {
        if (outerHalf <= innerHalf)
            return;

        const int thickness = outerHalf - innerHalf;
        std::vector<VkClearRect> rects;
        rects.reserve(4);
        AppendClippedClearRect(renderer, rects, cx - outerHalf, cy - outerHalf, outerHalf * 2 + 1, thickness);
        AppendClippedClearRect(renderer, rects, cx - outerHalf, cy + innerHalf + 1, outerHalf * 2 + 1, thickness);
        AppendClippedClearRect(renderer, rects, cx - outerHalf, cy - innerHalf, thickness, innerHalf * 2 + 1);
        AppendClippedClearRect(renderer, rects, cx + innerHalf + 1, cy - innerHalf, thickness, innerHalf * 2 + 1);
        CmdClearRects(renderer, commandBuffer, red, green, blue, alpha, rects);
    }

    void CmdClearCompassFrameBase(VulkanMinimapRenderer& renderer, void* commandBuffer, int cx, int cy, int radius)
    {
        CmdClearSquareBand(renderer, commandBuffer, 0.0f, 0.0f, 0.0f, 0.50f, cx + 2, cy + 3, radius + 15, radius + 2);
        CmdClearSquareBand(renderer, commandBuffer, 0.016f, 0.015f, 0.014f, 1.0f, cx, cy, radius + 13, radius + 1);
        CmdClearSquareBand(renderer, commandBuffer, 0.086f, 0.072f, 0.054f, 1.0f, cx, cy, radius + 11, radius + 6);
    }

    void CmdClearCompassFrameOverlay(VulkanMinimapRenderer& renderer, void* commandBuffer, int cx, int cy, int radius)
    {
        CmdClearSquareBand(renderer, commandBuffer, 0.90f, 0.71f, 0.38f, 1.0f, cx, cy, radius + 14, radius + 13);
        CmdClearSquareBand(renderer, commandBuffer, 0.25f, 0.17f, 0.085f, 1.0f, cx, cy, radius + 11, radius + 10);
        CmdClearSquareBand(renderer, commandBuffer, 0.76f, 0.56f, 0.28f, 1.0f, cx, cy, radius + 5, radius + 4);
        CmdClearSquareBand(renderer, commandBuffer, 0.96f, 0.78f, 0.45f, 1.0f, cx, cy, radius + 1, radius);

        const int badge = radius + 13;
        CmdClearCompassBadge(renderer, commandBuffer, cx, cy - badge, 'N');
        CmdClearCompassBadge(renderer, commandBuffer, cx + badge, cy, 'E');
        CmdClearCompassBadge(renderer, commandBuffer, cx, cy + badge, 'S');
        CmdClearCompassBadge(renderer, commandBuffer, cx - badge, cy, 'W');

        const int jewel = radius + 9;
        CmdClearRimJewel(renderer, commandBuffer, cx - jewel, cy - jewel);
        CmdClearRimJewel(renderer, commandBuffer, cx + jewel, cy - jewel);
        CmdClearRimJewel(renderer, commandBuffer, cx - jewel, cy + jewel);
        CmdClearRimJewel(renderer, commandBuffer, cx + jewel, cy + jewel);
    }

    void DrawMinimapWidget(VulkanMinimapRenderer& renderer, void* commandBuffer)
    {
        const int shortEdge = static_cast<int>(MinValue(renderer.width, renderer.height));
        const int autoRadius = ClampValue(shortEdge / 11, 104, 142);
        const int customRadius = g_layoutRadius.load();
        const int radius = customRadius > 0 ? ClampValue(customRadius, LAYOUT_RADIUS_MIN, LAYOUT_RADIUS_MAX) : autoRadius;
        const int marginX = MaxValue(58, static_cast<int>(renderer.width) / 58);
        const int marginY = MaxValue(52, static_cast<int>(renderer.height) / 42);
        const int layoutHalf = radius + MinimapRasterFrameExtra(radius);
        const int cx = ClampValue(static_cast<int>(renderer.width) - marginX - autoRadius + g_layoutOffsetX.load(),
            layoutHalf, MaxValue(layoutHalf, static_cast<int>(renderer.width) - layoutHalf));
        const int cy = ClampValue(ComputeMinimapCenterY(renderer.height, autoRadius, marginY) + g_layoutOffsetY.load(),
            layoutHalf, MaxValue(layoutHalf, static_cast<int>(renderer.height) - layoutHalf));
        g_layoutRectCx.store(cx);
        g_layoutRectCy.store(cy);
        g_layoutRectHalf.store(layoutHalf);
        g_layoutRectRadius.store(radius);
        g_layoutRectFrameExtra.store(MinimapRasterFrameExtra(radius));
        g_layoutScreenWidth.store(renderer.width);
        g_layoutScreenHeight.store(renderer.height);
        const int zoomStep = ClampValue(g_minimapZoomStep.load(), -3, 7);
        const float zoom = std::pow(1.32f, static_cast<float>(zoomStep));
        const float unitsPerPixel = MINIMAP_BASE_UNITS_PER_PIXEL / zoom;

        const std::vector<CapturedWaypoint> waypoints = CopyWaypoints();
        const std::vector<CapturedNearbyMarker> nearbyMarkers = CopyNearbyMarkers();
        const std::vector<CapturedMapMarkerVisibility> visibleMapMarkers = CopyVisibleMapMarkers();
        std::vector<MinimapWorldPoint> points;
        BuildWorldPoints(points, waypoints, nearbyMarkers, visibleMapMarkers);

        CapturedPlayerPosition playerPosition{};
        if (!TryGetPlayerPosition(playerPosition))
            return;

        const float playerX = FixedToWorld(playerPosition.x);
        const float playerZ = FixedToWorld(playerPosition.z);
        float centerX = playerX;
        float centerZ = playerZ;

        // Frame-rate heading smoothing: camera reads arrive irregularly (~20-40 Hz from
        // the background hold), so ease the drawn rotation toward the target each frame
        // instead of stepping on every publish.
        static float smoothedHeading = 0.0f;
        static DWORD smoothedHeadingTick = 0;
        static bool smoothedHeadingValid = false;
        float mapHeading = playerPosition.hasHeading ? playerPosition.headingRadians : 0.0f;
        const DWORD headingNow = GetTickCount();
        if (!playerPosition.hasHeading || !smoothedHeadingValid || headingNow - smoothedHeadingTick > 1000)
        {
            smoothedHeading = mapHeading;
            smoothedHeadingValid = playerPosition.hasHeading;
        }
        else
        {
            const float dtSeconds = static_cast<float>(MinValue<DWORD>(headingNow - smoothedHeadingTick, 250)) / 1000.0f;
            // The movement heading moves in steps (one per ~0.5 m walked), so the drawn
            // arrow eases toward it.
            const float alpha = 1.0f - std::exp(-dtSeconds / 0.09f);
            smoothedHeading = WrapAngleRadians(smoothedHeading + WrapAngleRadians(mapHeading - smoothedHeading) * alpha);
        }
        smoothedHeadingTick = headingNow;
        mapHeading = smoothedHeading;

        // Always north-up: the map, frame and markers never rotate; the arrow turns
        // instead. The camera heading h is the angle that used to rotate the map so the
        // facing direction pointed up, so on a north-up map the facing points at
        // (sin h, -cos h), i.e. the upright arrow art rotated clockwise by h.
        const bool staticView = g_minimapStaticView.load();
        const float arrowRotation = playerPosition.hasHeading ? mapHeading : 0.0f;
        mapHeading = 0.0f;

        // Static view: the map stays put and the arrow walks across it. Once the arrow
        // gets within ~40% of the rim the view glides back onto the player.
        static float viewX = 0.0f;
        static float viewZ = 0.0f;
        static bool viewValid = false;
        static bool viewRecentering = false;
        static DWORD viewTick = 0;
        if (staticView)
        {
            const float mapRadiusWorld = unitsPerPixel * static_cast<float>(radius);
            const DWORD viewNow = GetTickCount();
            float offsetX = playerX - viewX;
            float offsetZ = playerZ - viewZ;
            float offset = MaxValue(std::fabs(offsetX), std::fabs(offsetZ));
            // First use, a teleport, or a long pause: snap onto the player.
            if (!viewValid || offset > mapRadiusWorld * 2.0f || viewNow - viewTick > 5000)
            {
                viewX = playerX;
                viewZ = playerZ;
                viewValid = true;
                viewRecentering = false;
                offsetX = 0.0f;
                offsetZ = 0.0f;
                offset = 0.0f;
            }

            if (offset > mapRadiusWorld * 0.60f)
                viewRecentering = true;

            if (viewRecentering)
            {
                const float dtSeconds = static_cast<float>(MinValue<DWORD>(viewNow - viewTick, 250)) / 1000.0f;
                const float alpha = 1.0f - std::exp(-dtSeconds / 0.18f);
                viewX += offsetX * alpha;
                viewZ += offsetZ * alpha;
                if (offset < mapRadiusWorld * 0.05f)
                    viewRecentering = false;
            }

            viewTick = viewNow;
            centerX = viewX;
            centerZ = viewZ;
        }
        else
        {
            viewValid = false;
            viewRecentering = false;
        }
        // x1.42: the square window reaches sqrt(2) further along its diagonals.
        AppendVisibleStaticPoiPoints(points, nearbyMarkers, visibleMapMarkers, centerX, centerZ, unitsPerPixel * static_cast<float>(radius) * STATIC_POI_DRAW_RADIUS_FACTOR * 1.42f);
        LimitMinimapWorldPoints(points, centerX, centerZ);

        const bool hasRasterFrame = HasLoadedMinimapFrame();
        const int frameExtra = MinimapRasterFrameExtra(radius);
        const int frameMapRadius = hasRasterFrame ? MinimapRasterMapRadius(radius, frameExtra) : radius - 6;
        if (!hasRasterFrame)
            CmdClearCompassFrameBase(renderer, commandBuffer, cx, cy, radius);

        if (!TryDrawRealMapGpu(renderer, commandBuffer, cx, cy, frameMapRadius, centerX, centerZ, unitsPerPixel, mapHeading))
            DrawRealMap(renderer, commandBuffer, cx, cy, frameMapRadius, centerX, centerZ, unitsPerPixel, mapHeading);
        TryDrawFogGpu(renderer, commandBuffer, cx, cy, frameMapRadius, centerX, centerZ, unitsPerPixel, mapHeading);

        if (hasRasterFrame)
        {
            if (!TryDrawMinimapTexturedFrame(renderer, commandBuffer, cx, cy, radius, frameExtra, mapHeading))
                TryDrawMinimapRasterFrame(renderer, commandBuffer, cx, cy, radius, frameExtra);
        }
        else
            CmdClearCompassFrameOverlay(renderer, commandBuffer, cx, cy, radius);

        // Draw order: map icons, then pings, then other players (with names), and the
        // waypoint (its icon or the outlined icon) on top of everything - including us.
        const auto drawRank = [](const MinimapWorldPoint& point) -> int
        {
            if (point.kind == MAP_MARKER_KEY_WAYPOINT || point.highlight != 0)
                return 3;
            if (point.kind == 13)
                return 2;
            if (point.kind == MAP_MARKER_KEY_PLAYER_PING)
                return 1;
            return 0;
        };
        std::stable_sort(points.begin(), points.end(), [&drawRank](const MinimapWorldPoint& left, const MinimapWorldPoint& right) {
            return drawRank(left) < drawRank(right);
        });
        std::size_t waypointStart = points.size();
        for (std::size_t index = 0; index < points.size(); ++index)
        {
            if (drawRank(points[index]) == 3)
            {
                waypointStart = index;
                break;
            }
        }

        // Our own marker goes right before the waypoint layer.
        int arrowX = cx;
        int arrowY = cy;
        if (staticView)
        {
            bool arrowClipped = false;
            ProjectWorldToMinimap(playerX, playerZ, centerX, centerZ, unitsPerPixel, mapHeading, cx, cy, radius, arrowX, arrowY, arrowClipped);
        }
        const float playerSize = ClampValue(static_cast<float>(radius) * 0.15f, 16.0f, 22.0f);
        const bool headingShown = g_headingEnabled.load();
        const auto drawOwnMarker = [&]()
        {
            if (!headingShown)
            {
                // View direction off (F11): a round lime dot, no rotation.
                if (!TryDrawSpriteGpu(renderer, commandBuffer, SPRITE_KEY_PLAYER_DOT, static_cast<float>(arrowX) + 0.5f, static_cast<float>(arrowY) + 0.5f, playerSize, 0.0f, 1.0f, cx, cy, frameMapRadius))
                    CmdClearSmallCircle(renderer, commandBuffer, cx, cy, frameMapRadius, arrowX, arrowY, static_cast<int>(playerSize * 0.42f), 0.62f, 0.95f, 0.22f, 1.0f);
                return;
            }
            if (!TryDrawSpriteGpu(renderer, commandBuffer, SPRITE_KEY_PLAYER, static_cast<float>(arrowX) + 0.5f, static_cast<float>(arrowY) + 0.5f, playerSize, arrowRotation, 1.0f, cx, cy, frameMapRadius))
                CmdClearPlayerTriangle(renderer, commandBuffer, arrowX, arrowY, playerSize, arrowRotation);
        };
        bool ownMarkerDrawn = false;

        const float iconSize = ClampValue(static_cast<float>(radius) * 0.2f, 22.0f, 30.0f);
        const int pointProjectionRadius = hasRasterFrame ? frameMapRadius : radius;
        const int pointClipRadius = hasRasterFrame ? frameMapRadius : radius - 14;
        for (std::size_t pointIndex = 0; pointIndex < points.size(); ++pointIndex)
        {
            const MinimapWorldPoint& point = points[pointIndex];
            if (pointIndex == waypointStart && !ownMarkerDrawn)
            {
                drawOwnMarker();
                ownMarkerDrawn = true;
            }
            int px = cx;
            int py = cy;
            bool clipped = false;
            ProjectWorldToMinimap(point.x, point.z, centerX, centerZ, unitsPerPixel, mapHeading, cx, cy, pointProjectionRadius, px, py, clipped);

            // Markers beyond the minimap radius used to pile up on the rim (dozens of
            // icons, a large FPS cost). Only player-authored/critical kinds stay pinned
            // to the edge: red waypoint flags and other players.
            const bool pinnedAtRim = point.kind == 11 || point.kind == 13 ||
                point.kind == MAP_MARKER_KEY_PLAYER_PING || point.kind == MAP_MARKER_KEY_WAYPOINT ||
                point.highlight != 0;
            if (clipped && !pinnedAtRim)
                continue;

            // NPCs: the game's figure icon in sky blue.
            if (point.kind == 12)
            {
                TryDrawSpriteGpu(renderer, commandBuffer, MAP_MARKER_KEY_NPC_FIGURE, static_cast<float>(px) + 0.5f, static_cast<float>(py) + 0.5f, iconSize * 0.8f, 0.0f, 1.0f, cx, cy, pointClipRadius, 0.45f, 0.80f, 1.00f);
                continue;
            }

            // Other players: sky-blue dot with the name underneath.
            if (point.kind == 13)
            {
                const float dotSize = clipped ? 12.0f : 15.0f;
                if (!TryDrawSpriteGpu(renderer, commandBuffer, SPRITE_KEY_ALLY, static_cast<float>(px) + 0.5f, static_cast<float>(py) + 0.5f, dotSize, 0.0f, 1.0f, cx, cy, pointClipRadius))
                    CmdClearSmallCircle(renderer, commandBuffer, cx, cy, pointClipRadius, px, py, static_cast<int>(dotSize / 2.0f), 0.40f, 0.80f, 1.0f, 1.0f);

                // Player name under the dot.
                if (point.label != 0)
                {
                    const float labelHeight = static_cast<float>(g_minimapLabelFontSize.load());
                    TryDrawSpriteGpu(renderer, commandBuffer, point.label,
                        static_cast<float>(px) + 0.5f,
                        static_cast<float>(py) + dotSize * 0.5f + labelHeight * 0.5f + 2.0f,
                        labelHeight, 0.0f, 1.0f, cx, cy, pointClipRadius);
                }
                continue;
            }

            float drawSize = clipped ? iconSize * 0.75f : iconSize;
            if (point.highlight != 0)
            {
                TryDrawSpriteGpu(renderer, commandBuffer, point.highlight,
                    static_cast<float>(px) + 0.5f, static_cast<float>(py) + 0.5f,
                    drawSize * IconSilhouetteDrawScale(point.kind), 0.0f, 1.0f, cx, cy, pointClipRadius);
            }
            const bool isPing = point.kind == MAP_MARKER_KEY_PLAYER_PING;
            if (isPing)
                g_pingPointsDrawn.fetch_add(1, std::memory_order_relaxed);
            // Pings draw in lime green, with the name of whoever set them underneath.
            const float tintRed = isPing ? 0.55f : 1.0f;
            const float tintGreen = isPing ? 1.00f : 1.0f;
            const float tintBlue = isPing ? 0.40f : 1.0f;
            if (TryDrawSpriteGpu(renderer, commandBuffer, point.kind, static_cast<float>(px) + 0.5f, static_cast<float>(py) + 0.5f, drawSize, 0.0f, 1.0f, cx, cy, pointClipRadius, tintRed, tintGreen, tintBlue))
            {
                if (isPing)
                {
                    g_pingSpriteDrawn.fetch_add(1, std::memory_order_relaxed);
                    if (point.label != 0)
                    {
                        const float labelHeight = static_cast<float>(g_minimapLabelFontSize.load());
                        TryDrawSpriteGpu(renderer, commandBuffer, point.label,
                            static_cast<float>(px) + 0.5f,
                            static_cast<float>(py) + drawSize * 0.5f + labelHeight * 0.5f,
                            labelHeight, 0.0f, 1.0f, cx, cy, pointClipRadius);
                    }
                }
                continue;
            }

            CmdClearPoiIcon(renderer, commandBuffer, cx, cy, pointClipRadius, px, py, point.kind, clipped);
        }

        if (!ownMarkerDrawn)
            drawOwnMarker();

        // Esc menu edit mode: a thin outline shows the drag area, the corner square resizes.
        if (g_layoutEditMode.load())
        {
            const int left = cx - layoutHalf;
            const int top = cy - layoutHalf;
            const int size = layoutHalf * 2;
            CmdClearRect(renderer, commandBuffer, 1.0f, 0.85f, 0.30f, 0.75f, left, top, size, 2);
            CmdClearRect(renderer, commandBuffer, 1.0f, 0.85f, 0.30f, 0.75f, left, top + size - 2, size, 2);
            CmdClearRect(renderer, commandBuffer, 1.0f, 0.85f, 0.30f, 0.75f, left, top, 2, size);
            CmdClearRect(renderer, commandBuffer, 1.0f, 0.85f, 0.30f, 0.75f, left + size - 2, top, 2, size);
            const int handle = LAYOUT_HANDLE_SIZE;
            CmdClearRect(renderer, commandBuffer, 0.08f, 0.07f, 0.03f, 0.9f, cx + layoutHalf - handle - 1, cy + layoutHalf - handle - 1, handle + 2, handle + 2);
            CmdClearRect(renderer, commandBuffer, 1.0f, 0.85f, 0.30f, 1.0f, cx + layoutHalf - handle, cy + layoutHalf - handle, handle, handle);
        }
    }

    bool RecordVulkanMinimapCommandLocked(std::uint32_t imageIndex)
    {
        if (!g_renderer.ready || imageIndex >= g_renderer.commandBuffers.size() || imageIndex >= g_renderer.framebuffers.size())
            return false;

        void* commandBuffer = reinterpret_cast<void*>(g_renderer.commandBuffers[imageIndex]);
        if (g_renderer.fns.resetCommandBuffer(commandBuffer, 0) != VK_SUCCESS)
            return false;

        // This image's fence was waited on before recording, so a map upload that was
        // submitted with it has completed and its staging memory can go.
        ReleaseGpuMapStagingIfUploadedLocked(g_renderer, imageIndex);
        DestroyRetiredGpuPipelinesIfSafeLocked(g_renderer, imageIndex);
        if (g_minimapMapGpuEnabled.load())
            TryCreateGpuMapResourcesLocked(g_renderer);
        TryCreateGpuSpriteResourcesLocked(g_renderer);
        TryCreateGpuFogResourcesLocked(g_renderer);
        UpdateGpuFogIfNeededLocked(g_renderer);

        VkCommandBufferBeginInfo beginInfo{};
        if (g_renderer.fns.beginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS)
            return false;

        // A fresh command buffer inherits no pipeline state.
        g_renderer.boundPipeline = 0;
        g_renderer.boundDescriptorSet = 0;

        RecordFrameTextureUploadIfNeeded(g_renderer, commandBuffer);
        RecordGpuMapUploadIfNeeded(g_renderer, commandBuffer, imageIndex);

        VkRenderPassBeginInfo renderPassBegin{};
        renderPassBegin.renderPass = reinterpret_cast<void*>(g_renderer.renderPass);
        renderPassBegin.framebuffer = reinterpret_cast<void*>(g_renderer.framebuffers[imageIndex]);
        renderPassBegin.renderArea.offset.x = 0;
        renderPassBegin.renderArea.offset.y = 0;
        renderPassBegin.renderArea.extent.width = g_renderer.width;
        renderPassBegin.renderArea.extent.height = g_renderer.height;

        g_renderer.fns.cmdBeginRenderPass(commandBuffer, &renderPassBegin, 0);
        DrawMinimapWidget(g_renderer, commandBuffer);
        g_renderer.fns.cmdEndRenderPass(commandBuffer);
        return g_renderer.fns.endCommandBuffer(commandBuffer) == VK_SUCCESS;
    }

    bool TryReadPresentTargets(const VkPresentInfoKHR& info, uintptr_t& firstSwapchain, std::uint32_t& imageIndex)
    {
        firstSwapchain = 0;
        imageIndex = 0;

        if (info.swapchainCount == 0 || info.swapchainCount > 8 || info.pSwapchains == nullptr || info.pImageIndices == nullptr)
            return false;

        return SafeReadValue(reinterpret_cast<uintptr_t>(info.pSwapchains), firstSwapchain) &&
            SafeReadValue(reinterpret_cast<uintptr_t>(info.pImageIndices), imageIndex) &&
            firstSwapchain != 0;
    }

    // The present hook runs on the game's render thread and calls the driver with our
    // cached Vulkan handles. When the game tears down and recreates its swapchain (scene
    // loads, resolution changes, and — crucially — handle-value reuse that defeats the
    // rebuild check), those handles reference destroyed framebuffers/image views and the
    // driver faults deep inside nvoglv64. SEH turns that fatal AV into a one-frame skip
    // plus a renderer rebuild, instead of taking the whole game down. This function must
    // contain no C++ objects needing unwinding (all such work lives in the callees).
    int RecordAndSubmitMinimapGuarded(void* queue, const VkSubmitInfo* submitInfo, void* commandFence, std::uint32_t imageIndex)
    {
        __try
        {
            if (!RecordVulkanMinimapCommandLocked(imageIndex))
                return -1;
            return static_cast<int>(g_renderer.fns.queueSubmit(queue, 1, submitInfo, commandFence));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1000;
        }
    }

    bool TrySubmitVulkanMinimap(void* queue, const VkPresentInfoKHR& info, uintptr_t firstSwapchain, std::uint32_t imageIndex, VkPresentInfoKHR& adjustedInfo, const void** adjustedWaitSemaphore)
    {
        if (queue == nullptr || firstSwapchain == 0 || info.waitSemaphoreCount > 8)
            return false;

        if (!g_minimapEnabled.load())
        {
            // First present after the switch went off: hand the map texture, the sprite
            // atlas and every other Vulkan object back to the driver. This runs on the
            // same thread that submits our work, so nothing of ours can still be in
            // flight behind it. Re-enabling rebuilds everything from scratch.
            if (g_minimapTeardownPending.exchange(false))
            {
                std::lock_guard<std::mutex> lock(g_rendererMutex);
                DestroyVulkanMinimapRendererLocked();
                Log("[Minimap] disabled: renderer and GPU textures released");
            }
            return false;
        }

        if (!ShouldDrawMinimapInWorld())
        {
            LogDrawGateThrottled();
            return false;
        }

        if (info.waitSemaphoreCount != 0 && info.pWaitSemaphores == nullptr)
            return false;

        SwapchainRuntimeInfo snapshot{};
        if (!TryGetSwapchainSnapshot(firstSwapchain, snapshot))
        {
            snapshot.handle = firstSwapchain;
            snapshot.device = g_lastVulkanDevice.load();
        }

        if (snapshot.device == 0)
            return false;

        std::lock_guard<std::mutex> lock(g_rendererMutex);
        if (!BuildVulkanMinimapRendererLocked(snapshot))
            return false;

        if (imageIndex >= g_renderer.commandBuffers.size() || imageIndex >= g_renderer.renderCompleteSemaphores.size() ||
            imageIndex >= g_renderer.commandFences.size())
        {
            return false;
        }

        // Wait until the GPU is done with this image's previous minimap submit before
        // resetting/re-recording its command buffer (device-lost guard). A miss just
        // skips the overlay for one frame.
        void* commandFence = reinterpret_cast<void*>(g_renderer.commandFences[imageIndex]);
        void* fenceDevice = reinterpret_cast<void*>(g_renderer.device);
        // This runs on the game's present thread, so every microsecond spent here is
        // frame time. Poll first (the fence is normally long signaled); only when the
        // GPU is genuinely behind do we wait, and then briefly - dropping the overlay
        // for one frame costs far less than stalling the whole game.
        std::int32_t fenceWait = g_renderer.fns.waitForFences(fenceDevice, 1, &commandFence, 1, 0ull);
        if (fenceWait != VK_SUCCESS)
            fenceWait = g_renderer.fns.waitForFences(fenceDevice, 1, &commandFence, 1, 1500000ull);
        if (fenceWait != VK_SUCCESS)
        {
            LogRendererThrottled("[Minimap] Vulkan minimap draw skipped: previous frame still in flight");
            return false;
        }

        if (g_renderer.fns.resetFences(fenceDevice, 1, &commandFence) != VK_SUCCESS)
        {
            LogRendererThrottled("[Minimap] Vulkan minimap draw skipped: fence reset failed");
            return false;
        }

        // Fixed-size: a per-frame heap allocation on the present thread is pure waste.
        constexpr std::uint32_t MAX_WAIT_SEMAPHORES = 16;
        if (info.waitSemaphoreCount > MAX_WAIT_SEMAPHORES)
            return false;
        std::uint32_t waitStages[MAX_WAIT_SEMAPHORES];
        for (std::uint32_t i = 0; i < info.waitSemaphoreCount; ++i)
            waitStages[i] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        const void* commandBuffer = reinterpret_cast<void*>(g_renderer.commandBuffers[imageIndex]);
        const void* signalSemaphore = reinterpret_cast<void*>(g_renderer.renderCompleteSemaphores[imageIndex]);

        VkSubmitInfo submitInfo{};
        submitInfo.waitSemaphoreCount = info.waitSemaphoreCount;
        submitInfo.pWaitSemaphores = info.pWaitSemaphores;
        submitInfo.pWaitDstStageMask = info.waitSemaphoreCount == 0 ? nullptr : waitStages;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &signalSemaphore;

        // Record + submit under SEH: a driver fault here (stale swapchain handles) must
        // not crash the game. On any failure the fence stayed unsignaled, so rebuild the
        // renderer to get a fresh signaled fence and valid handles next frame.
        const int submitResult = RecordAndSubmitMinimapGuarded(queue, &submitInfo, commandFence, imageIndex);
        if (submitResult != VK_SUCCESS)
        {
            std::ostringstream oss;
            oss << "[Minimap] Vulkan minimap draw skipped: record/submit failed"
                << " | result=" << submitResult
                << " | image_index=" << imageIndex;
            LogRendererThrottled(oss.str());
            DestroyVulkanMinimapRendererLocked();
            return false;
        }

        adjustedWaitSemaphore[0] = signalSemaphore;
        adjustedInfo = info;
        adjustedInfo.waitSemaphoreCount = 1;
        adjustedInfo.pWaitSemaphores = adjustedWaitSemaphore;
        return true;
    }

    // ---- LAYOUT EDIT MODE (Esc menu: drag to move, corner handle to resize) ----
    // Pressing Esc while playing opens the game menu and shows the cursor; from then until
    // the cursor hides again the minimap stays on screen and can be dragged. The bottom
    // right corner carries a handle that resizes it. The result is kept in
    // mods\minimap_mod\minimap_layout.txt ("offset_x offset_y radius") and applied on load.
    constexpr const char* LAYOUT_FILE_NAME = "mods\\minimap_mod\\minimap_layout.txt";

    std::atomic<bool> g_layoutLoaded{ false };
    std::atomic<DWORD> g_layoutEscapeTick{ 0 };   // last Esc press
    bool g_layoutEscapeWasDown = false;             // input thread only

    int g_layoutDragMode = 0;   // 0 none, 1 move, 2 resize (input thread only)
    int g_layoutDragLastX = 0;
    int g_layoutDragLastY = 0;
    bool g_layoutDirty = false;

    std::string LayoutFilePath()
    {
        return JoinPath(GetExecutableDirectory(), LAYOUT_FILE_NAME);
    }

    void EnsureLayoutLoaded()
    {
        if (g_layoutLoaded.exchange(true))
            return;
        std::ifstream file(LayoutFilePath());
        int offsetX = 0;
        int offsetY = 0;
        int radius = 0;
        if (file && (file >> offsetX >> offsetY >> radius))
        {
            g_layoutOffsetX.store(ClampValue(offsetX, -8192, 8192));
            g_layoutOffsetY.store(ClampValue(offsetY, -8192, 8192));
            g_layoutRadius.store(radius > 0 ? ClampValue(radius, LAYOUT_RADIUS_MIN, LAYOUT_RADIUS_MAX) : 0);
            std::ostringstream oss;
            oss << "[Minimap] layout loaded | offset=(" << g_layoutOffsetX.load() << "," << g_layoutOffsetY.load()
                << ") | radius=" << g_layoutRadius.load();
            Log(oss.str());
        }
    }

    void SaveLayout()
    {
        std::ofstream file(LayoutFilePath(), std::ios::trunc);
        if (!file)
        {
            Log("[Minimap] layout not saved (cannot write mods\\minimap_mod\\minimap_layout.txt)");
            return;
        }
        file << g_layoutOffsetX.load() << " " << g_layoutOffsetY.load() << " " << g_layoutRadius.load() << "\n";
        std::ostringstream oss;
        oss << "[Minimap] layout saved | offset=(" << g_layoutOffsetX.load() << "," << g_layoutOffsetY.load()
            << ") | radius=" << g_layoutRadius.load();
        Log(oss.str());
    }

    bool TryGetCursorInGameWindow(int& outX, int& outY)
    {
        HWND window = GetForegroundWindow();
        if (window == nullptr)
            return false;
        DWORD processId = 0;
        GetWindowThreadProcessId(window, &processId);
        if (processId != GetCurrentProcessId())
            return false;
        POINT cursor{};
        if (!GetCursorPos(&cursor) || !ScreenToClient(window, &cursor))
            return false;
        RECT client{};
        if (!GetClientRect(window, &client))
            return false;
        // The swapchain may be larger or smaller than the window (scaling): map through it.
        const int clientWidth = MaxValue(1, static_cast<int>(client.right - client.left));
        const int clientHeight = MaxValue(1, static_cast<int>(client.bottom - client.top));
        const std::uint32_t screenWidth = g_layoutScreenWidth.load();
        const std::uint32_t screenHeight = g_layoutScreenHeight.load();
        if (screenWidth == 0 || screenHeight == 0)
            return false;
        outX = static_cast<int>(static_cast<std::int64_t>(cursor.x) * static_cast<std::int64_t>(screenWidth) / clientWidth);
        outY = static_cast<int>(static_cast<std::int64_t>(cursor.y) * static_cast<std::int64_t>(screenHeight) / clientHeight);
        return true;
    }

    // Called every frame from the render hook.
    void UpdateMinimapLayoutEdit()
    {
        EnsureLayoutLoaded();

        // Esc edge from the "currently down" bit: the "pressed since last call" bit is
        // shared with the game and usually already consumed. The cursor only shows a few
        // frames after the press, so the press is remembered for a moment.
        const DWORD now = GetTickCount();
        CURSORINFO cursorInfo{};
        cursorInfo.cbSize = sizeof(cursorInfo);
        const bool cursorShowing = GetCursorInfo(&cursorInfo) && (cursorInfo.flags & CURSOR_SHOWING) != 0;

        // Only an Esc pressed while the cursor is hidden opens the menu; an Esc pressed
        // with the cursor showing closes something, and must not arm edit mode for the
        // next screen (e.g. the inventory opened right after).
        const bool escapeDown = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
        const bool escapePressed = (escapeDown && !g_layoutEscapeWasDown) || (GetAsyncKeyState(VK_ESCAPE) & 0x0001) != 0;
        if (escapePressed)
            g_layoutEscapeTick.store(cursorShowing ? 0 : now);
        g_layoutEscapeWasDown = escapeDown;

        if (!cursorShowing)
        {
            if (g_layoutEditMode.exchange(false))
            {
                if (g_layoutDirty)
                {
                    g_layoutDirty = false;
                    SaveLayout();
                }
                Log("[Minimap] layout edit mode off");
            }
            g_layoutDragMode = 0;
            return;
        }
        const DWORD escapeTick = g_layoutEscapeTick.load();
        if (!g_layoutEditMode.load() && escapeTick != 0 && TicksSince(now, escapeTick) < 1500)
        {
            g_layoutEscapeTick.store(0);
            g_layoutEditMode.store(true);
            Log("[Minimap] layout edit mode on (drag the minimap to move it, the corner square to resize)");
        }
        if (!g_layoutEditMode.load())
            return;

        const bool buttonDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        int mouseX = 0;
        int mouseY = 0;
        if (!buttonDown || !TryGetCursorInGameWindow(mouseX, mouseY))
        {
            if (g_layoutDragMode != 0 && g_layoutDirty)
            {
                g_layoutDirty = false;
                SaveLayout();
            }
            g_layoutDragMode = 0;
            return;
        }

        const int cx = g_layoutRectCx.load();
        const int cy = g_layoutRectCy.load();
        const int half = g_layoutRectHalf.load();
        if (g_layoutDragMode == 0)
        {
            const bool onHandle = std::abs(mouseX - (cx + half)) <= LAYOUT_HANDLE_SIZE && std::abs(mouseY - (cy + half)) <= LAYOUT_HANDLE_SIZE;
            const bool inside = std::abs(mouseX - cx) <= half && std::abs(mouseY - cy) <= half;
            if (onHandle)
                g_layoutDragMode = 2;
            else if (inside)
                g_layoutDragMode = 1;
            else
                return;
            g_layoutDragLastX = mouseX;
            g_layoutDragLastY = mouseY;
            return;
        }

        const int deltaX = mouseX - g_layoutDragLastX;
        const int deltaY = mouseY - g_layoutDragLastY;
        g_layoutDragLastX = mouseX;
        g_layoutDragLastY = mouseY;
        if (deltaX == 0 && deltaY == 0)
            return;

        if (g_layoutDragMode == 1)
        {
            g_layoutOffsetX.store(g_layoutOffsetX.load() + deltaX);
            g_layoutOffsetY.store(g_layoutOffsetY.load() + deltaY);
            g_layoutDirty = true;
        }
        else
        {
            // The corner follows the cursor: half size = radius + frame margin.
            const int wantedHalf = MaxValue(std::abs(mouseX - cx), std::abs(mouseY - cy));
            const int frameExtra = g_layoutRectFrameExtra.load();
            const int radius = ClampValue(wantedHalf - frameExtra, LAYOUT_RADIUS_MIN, LAYOUT_RADIUS_MAX);
            if (radius != g_layoutRectRadius.load())
            {
                g_layoutRadius.store(radius);
                g_layoutDirty = true;
            }
        }
    }
    // ---- END LAYOUT EDIT MODE ----

    void UpdateMinimapZoomHotkeys()
    {
        int step = g_minimapZoomStep.load();
        bool changed = false;
        if ((GetAsyncKeyState(VK_ADD) & 0x0001) != 0 ||
            (GetAsyncKeyState(VK_OEM_PLUS) & 0x0001) != 0)
        {
            step = ClampValue(step + 1, -3, 7);
            changed = true;
        }

        if ((GetAsyncKeyState(VK_SUBTRACT) & 0x0001) != 0 ||
            (GetAsyncKeyState(VK_OEM_MINUS) & 0x0001) != 0)
        {
            step = ClampValue(step - 1, -3, 3);
            changed = true;
        }

        if (changed)
        {
            g_minimapZoomStep.store(step);
            std::ostringstream oss;
            oss << "[Minimap] zoom step=" << step;
            Log(oss.str());
        }
    }

    void UpdateMinimapVisibilityHotkey()
    {
        const int toggleKey = g_minimapToggleKey.load();
        const bool configuredPressed = toggleKey != 0 && (GetAsyncKeyState(toggleKey) & 0x0001) != 0;
        const bool legacyPressed = toggleKey != VK_MULTIPLY && (GetAsyncKeyState(VK_MULTIPLY) & 0x0001) != 0;
        if (!configuredPressed && !legacyPressed)
            return;

        const bool enabled = !g_minimapEnabled.load();
        g_minimapEnabled.store(enabled);
        g_minimapVisible.store(enabled);
        if (!enabled)
        {
            // Picked up by the next present, which is where Vulkan teardown is safe.
            g_minimapTeardownPending.store(true);
        }
        else
        {
            g_minimapTeardownPending.store(false);
            // Come back with a clean slate: the map image was dropped from RAM after its
            // upload, so let it reload, and rebuild the name-label atlas.
            RequestSpriteAtlasRebuild();
        }

        std::ostringstream oss;
        oss << "[Minimap] " << (enabled ? "enabled" : "disabled (hooks idle, GPU resources freed)")
            << " | key=" << (configuredPressed ? MinimapToggleKeyName(toggleKey) : "Numpad *");
        Log(oss.str());
    }

    void UpdateHeadingToggleHotkey()
    {
        const int key = g_headingToggleKey.load();
        if (key == 0 || (GetAsyncKeyState(key) & 0x0001) == 0)
            return;

        const bool enabled = !g_headingEnabled.load();
        g_headingEnabled.store(enabled);
        // Start clean either way: no stale direction from before the switch.
        ResetHeadingState();

        std::ostringstream oss;
        oss << "[Minimap] view direction " << (enabled ? "on" : "off (own marker: dot)")
            << " | key=" << MinimapToggleKeyName(key);
        Log(oss.str());
    }

    std::mutex g_runtimeControlsMutex;

    // Called from Shroudtopia's update thread and from the present hook. The two must
    // not run this at the same time (the layout-edit state is plain data), and the
    // config file is only re-read from the update thread so the render thread never
    // touches disk.
    void UpdateMinimapRuntimeControls(ModContext* modContext, bool allowConfigRefresh)
    {
        std::unique_lock<std::mutex> lock(g_runtimeControlsMutex, std::try_to_lock);
        if (!lock.owns_lock())
            return;

        // The toggle key is the one thing that still runs while the mod is off.
        UpdateMinimapVisibilityHotkey();
        if (!g_minimapEnabled.load())
            return;

        const DWORD now = GetTickCount();
        if (allowConfigRefresh && now - g_lastConfigPollTick >= MINIMAP_CONFIG_POLL_MS)
        {
            g_lastConfigPollTick = now;
            RefreshMinimapConfig(modContext);
        }

        UpdateHeadingToggleHotkey();
        UpdateMinimapZoomHotkeys();
        UpdateMinimapLayoutEdit();
    }

    void LogVulkanPresentInfo(void* queue, const void* presentInfo)
    {
        if (!g_debugLoggingEnabled.load())
            return;

        const DWORD now = GetTickCount();
        if (now - g_lastVulkanPresentInfoTick < 5000)
            return;

        g_lastVulkanPresentInfoTick = now;

        VkPresentInfoKHR info{};
        const bool hasPresentInfo = presentInfo != nullptr &&
            SafeReadValue(reinterpret_cast<uintptr_t>(presentInfo), info);

        uintptr_t firstSwapchain = 0;
        std::uint32_t imageIndex = 0;
        bool hasImageIndex = false;
        if (hasPresentInfo && info.swapchainCount != 0 && info.swapchainCount <= 8)
        {
            SafeReadValue(reinterpret_cast<uintptr_t>(info.pSwapchains), firstSwapchain);
            if (info.pImageIndices != nullptr)
                hasImageIndex = SafeReadValue(reinterpret_cast<uintptr_t>(info.pImageIndices), imageIndex);
        }

        SwapchainRuntimeInfo swapchain{};
        const bool knownSwapchain = firstSwapchain != 0 && TryGetSwapchainSnapshot(firstSwapchain, swapchain);

        std::ostringstream oss;
        oss << "[Minimap] vkQueuePresentKHR hook"
            << " | queue=" << Hex(reinterpret_cast<uintptr_t>(queue))
            << " | present_info=" << Hex(reinterpret_cast<uintptr_t>(presentInfo))
            << " | render_context=" << Hex(g_lastRenderContext.load())
            << " | device=" << Hex(g_lastVulkanDevice.load())
            << " | graphics_context=" << Hex(g_lastGraphicsContext.load())
            << " | swapchain_state=" << Hex(g_lastSwapchainState.load());

        if (hasPresentInfo)
        {
            oss << " | wait_semaphores=" << info.waitSemaphoreCount
                << " | swapchain_count=" << info.swapchainCount
                << " | first_swapchain=" << Hex(firstSwapchain);
            if (hasImageIndex)
                oss << " | image_index=" << imageIndex;
        }
        else
        {
            oss << " | present_info_read=failed";
        }

        if (knownSwapchain)
        {
            oss << " | swapchain_format=" << swapchain.format
                << " | extent=" << swapchain.width << "x" << swapchain.height
                << " | images=" << swapchain.imageCount
                << " | cached_images=" << swapchain.images.size();
        }

        Log(oss.str());
    }

    void LogVulkanSwapchainCreate(void* device, const VkSwapchainCreateInfoKHR* createInfo, void* swapchain, std::int32_t result)
    {
        if (!g_debugLoggingEnabled.load())
            return;

        const DWORD now = GetTickCount();
        if (now - g_lastVulkanSwapchainHookTick < 5000)
            return;

        g_lastVulkanSwapchainHookTick = now;

        VkSwapchainCreateInfoKHR info{};
        const bool hasCreateInfo = TryCopySwapchainCreateInfo(createInfo, info);

        std::ostringstream oss;
        oss << "[Minimap] vkCreateSwapchainKHR"
            << " | result=" << result
            << " | device=" << Hex(reinterpret_cast<uintptr_t>(device))
            << " | swapchain=" << Hex(reinterpret_cast<uintptr_t>(swapchain));
        if (hasCreateInfo)
        {
            oss << " | format=" << info.imageFormat
                << " | extent=" << info.imageExtent.width << "x" << info.imageExtent.height
                << " | min_images=" << info.minImageCount
                << " | present_mode=" << info.presentMode
                << " | sharing_mode=" << info.imageSharingMode
                << " | queue_family_count=" << info.queueFamilyIndexCount;
        }
        Log(oss.str());
    }

    std::int32_t __fastcall HookCreateSwapchain(void* device, const VkSwapchainCreateInfoKHR* createInfo, const void* allocator, void** swapchain)
    {
        if (device != nullptr)
            g_lastVulkanDevice.store(reinterpret_cast<uintptr_t>(device));

        const uintptr_t original = g_originalCreateSwapchain.load();
        auto* originalFn = reinterpret_cast<CreateSwapchainFn>(original);
        const std::int32_t result = originalFn != nullptr ? originalFn(device, createInfo, allocator, swapchain) : -1;
        void* swapchainHandle = swapchain != nullptr ? *swapchain : nullptr;
        RememberSwapchainCreate(device, createInfo, swapchainHandle, result);
        LogVulkanSwapchainCreate(device, createInfo, swapchainHandle, result);
        return result;
    }

    std::int32_t __fastcall HookGetSwapchainImages(void* device, void* swapchain, std::uint32_t* count, void* images)
    {
        if (device != nullptr)
            g_lastVulkanDevice.store(reinterpret_cast<uintptr_t>(device));

        const uintptr_t original = g_originalGetSwapchainImages.load();
        auto* originalFn = reinterpret_cast<GetSwapchainImagesFn>(original);
        const std::int32_t result = originalFn != nullptr ? originalFn(device, swapchain, count, images) : -1;
        RememberSwapchainImages(device, swapchain, count, images, result);

        const DWORD now = GetTickCount();
        if (now - g_lastVulkanSwapchainImagesHookTick >= 5000)
        {
            g_lastVulkanSwapchainImagesHookTick = now;
            std::uint32_t imageCount = 0;
            if (count != nullptr)
                SafeReadValue(reinterpret_cast<uintptr_t>(count), imageCount);

            std::ostringstream oss;
            oss << "[Minimap] vkGetSwapchainImagesKHR"
                << " | result=" << result
                << " | device=" << Hex(reinterpret_cast<uintptr_t>(device))
                << " | swapchain=" << Hex(reinterpret_cast<uintptr_t>(swapchain))
                << " | count=" << imageCount
                << " | images_ptr=" << Hex(reinterpret_cast<uintptr_t>(images));
            Log(oss.str());
        }

        return result;
    }

    std::int32_t __fastcall HookQueuePresent(void* queue, const void* presentInfo)
    {
        UpdateMinimapRuntimeControls(g_modContext, false);
        if (g_minimapEnabled.load())
        {
            TryScanVulkanDeviceFunctions();
            LogVulkanPresentInfo(queue, presentInfo);
        }

        const uintptr_t original = g_originalQueuePresent.load();
        auto* originalFn = reinterpret_cast<QueuePresentFn>(original);
        if (originalFn == nullptr)
            return -1;

        VkPresentInfoKHR info{};
        uintptr_t firstSwapchain = 0;
        std::uint32_t imageIndex = 0;
        VkPresentInfoKHR adjustedInfo{};
        const void* adjustedWaitSemaphore[1] = {};

        const bool canDraw = presentInfo != nullptr &&
            SafeReadValue(reinterpret_cast<uintptr_t>(presentInfo), info) &&
            TryReadPresentTargets(info, firstSwapchain, imageIndex) &&
            TrySubmitVulkanMinimap(queue, info, firstSwapchain, imageIndex, adjustedInfo, adjustedWaitSemaphore);

        return originalFn(queue, canDraw ? &adjustedInfo : presentInfo);
    }

    bool HookVulkanTableFunction(uintptr_t table, std::size_t offset, void* hook, std::atomic<uintptr_t>& original, const char* label)
    {
        const uintptr_t slot = table + offset;
        uintptr_t current = 0;
        if (!SafeReadValue(slot, current) || current == 0)
            return false;

        const uintptr_t hookAddress = reinterpret_cast<uintptr_t>(hook);
        if (current == hookAddress)
            return original.load() != 0;

        uintptr_t expectedZero = 0;
        original.compare_exchange_strong(expectedZero, current);

        if (!WritePointer(slot, hookAddress))
        {
            std::ostringstream oss;
            oss << "[Minimap] failed to hook Vulkan table " << label
                << " at " << Hex(slot);
            Log(oss.str());
            return false;
        }

        return true;
    }

    bool TryInstallVulkanTableHooks(uintptr_t table)
    {
        if (table == 0)
            return false;

        std::lock_guard<std::mutex> lock(g_vulkanHookMutex);
        if (g_patchedVulkanDeviceTable.load() == table &&
            g_originalQueuePresent.load() != 0 &&
            g_originalCreateSwapchain.load() != 0 &&
            g_originalGetSwapchainImages.load() != 0)
        {
            return true;
        }

        const bool createOk = HookVulkanTableFunction(
            table,
            VULKAN_TABLE_CREATE_SWAPCHAIN_OFFSET,
            reinterpret_cast<void*>(&HookCreateSwapchain),
            g_originalCreateSwapchain,
            "vkCreateSwapchainKHR"
        );
        const bool imagesOk = HookVulkanTableFunction(
            table,
            VULKAN_TABLE_GET_SWAPCHAIN_IMAGES_OFFSET,
            reinterpret_cast<void*>(&HookGetSwapchainImages),
            g_originalGetSwapchainImages,
            "vkGetSwapchainImagesKHR"
        );
        const bool presentOk = HookVulkanTableFunction(
            table,
            VULKAN_TABLE_QUEUE_PRESENT_OFFSET,
            reinterpret_cast<void*>(&HookQueuePresent),
            g_originalQueuePresent,
            "vkQueuePresentKHR"
        );

        if (createOk || imagesOk || presentOk)
            g_patchedVulkanDeviceTable.store(table);

        const DWORD now = GetTickCount();
        if (now - g_lastVulkanHookSummaryTick >= 5000)
        {
            g_lastVulkanHookSummaryTick = now;
            std::ostringstream oss;
            oss << "[Minimap] Vulkan table hooks"
                << " | table=" << Hex(table)
                << " | create_swapchain=" << (createOk ? "hooked" : "missing")
                << " | get_swapchain_images=" << (imagesOk ? "hooked" : "missing")
                << " | queue_present=" << (presentOk ? "hooked" : "missing")
                << " | no_external_overlay=true";
            Log(oss.str());
        }

        return createOk && imagesOk && presentOk;
    }

    void RestoreVulkanTableFunction(uintptr_t table, std::size_t offset, void* hook, std::atomic<uintptr_t>& original)
    {
        const uintptr_t originalValue = original.load();
        if (table == 0 || originalValue == 0)
            return;

        const uintptr_t slot = table + offset;
        uintptr_t current = 0;
        if (!SafeReadValue(slot, current))
            return;

        if (current == reinterpret_cast<uintptr_t>(hook))
            WritePointer(slot, originalValue);

        original.store(0);
    }

    void RestoreVulkanTableHooks()
    {
        std::lock_guard<std::mutex> lock(g_vulkanHookMutex);
        const uintptr_t table = g_patchedVulkanDeviceTable.load();
        RestoreVulkanTableFunction(table, VULKAN_TABLE_CREATE_SWAPCHAIN_OFFSET, reinterpret_cast<void*>(&HookCreateSwapchain), g_originalCreateSwapchain);
        RestoreVulkanTableFunction(table, VULKAN_TABLE_GET_SWAPCHAIN_IMAGES_OFFSET, reinterpret_cast<void*>(&HookGetSwapchainImages), g_originalGetSwapchainImages);
        RestoreVulkanTableFunction(table, VULKAN_TABLE_QUEUE_PRESENT_OFFSET, reinterpret_cast<void*>(&HookQueuePresent), g_originalQueuePresent);
        g_patchedVulkanDeviceTable.store(0);
    }

    bool LooksLikeVulkanDeviceTable(uintptr_t candidate, uintptr_t vulkanModule, uintptr_t vkGetInstanceProcAddr)
    {
        uintptr_t module = 0;
        uintptr_t getInstanceProcAddr = 0;
        uintptr_t createInstance = 0;
        uintptr_t getDeviceProcAddr = 0;
        uintptr_t createSwapchain = 0;
        uintptr_t queuePresent = 0;

        if (!SafeReadValue(candidate + 0x10, module) ||
            !SafeReadValue(candidate + 0x18, getInstanceProcAddr) ||
            !SafeReadValue(candidate + 0x20, createInstance) ||
            !SafeReadValue(candidate + 0x78, getDeviceProcAddr) ||
            !SafeReadValue(candidate + VULKAN_TABLE_CREATE_SWAPCHAIN_OFFSET, createSwapchain) ||
            !SafeReadValue(candidate + VULKAN_TABLE_QUEUE_PRESENT_OFFSET, queuePresent))
        {
            return false;
        }

        return module == vulkanModule &&
            getInstanceProcAddr == vkGetInstanceProcAddr &&
            createInstance != 0 &&
            getDeviceProcAddr != 0 &&
            createSwapchain != 0 &&
            queuePresent != 0;
    }

    // Finds the game's Vulkan device dispatch table by scanning private memory. This
    // used to run on the Shroudtopia thread inside Activate() with six guarded reads
    // per 8-byte step: on a cold start (table not created yet) it could crawl through
    // the whole address space for minutes, and Update() - which drives the camera scan
    // (view direction), session detection and more - never ran. It now reads memory in
    // chunks, bails out as soon as the render hook has found the table, and runs on its
    // own thread.
    bool ScanForVulkanDeviceTable()
    {
        HMODULE vulkanModule = GetModuleHandleA("vulkan-1.dll");
        if (vulkanModule == nullptr)
            return false;

        FARPROC getInstanceProcAddr = GetProcAddress(vulkanModule, "vkGetInstanceProcAddr");
        if (getInstanceProcAddr == nullptr)
            return false;

        const uintptr_t moduleValue = reinterpret_cast<uintptr_t>(vulkanModule);
        const uintptr_t procValue = reinterpret_cast<uintptr_t>(getInstanceProcAddr);

        SYSTEM_INFO systemInfo{};
        GetSystemInfo(&systemInfo);

        uintptr_t cursor = reinterpret_cast<uintptr_t>(systemInfo.lpMinimumApplicationAddress);
        const uintptr_t maximum = reinterpret_cast<uintptr_t>(systemInfo.lpMaximumApplicationAddress);
        constexpr std::size_t TABLE_MIN_SIZE = VULKAN_TABLE_QUEUE_PRESENT_OFFSET + sizeof(uintptr_t);
        constexpr std::size_t CHUNK = 0x10000;
        std::vector<std::uint8_t> buffer(CHUNK);

        MEMORY_BASIC_INFORMATION info{};
        while (cursor < maximum && VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof(info)) == sizeof(info))
        {
            if (g_vulkanDeviceTable.load() != 0)
                return true;

            const uintptr_t regionBase = reinterpret_cast<uintptr_t>(info.BaseAddress);
            const uintptr_t next = regionBase + info.RegionSize;

            if (info.State == MEM_COMMIT &&
                info.Type == MEM_PRIVATE &&
                info.RegionSize >= TABLE_MIN_SIZE &&
                IsReadablePage(info.Protect))
            {
                for (uintptr_t chunkBase = regionBase; chunkBase < next; chunkBase += CHUNK - 0x20)
                {
                    const std::size_t chunkSize = static_cast<std::size_t>(MinValue<uintptr_t>(CHUNK, next - chunkBase));
                    if (chunkSize < 0x20 || !SafeRead(chunkBase, buffer.data(), chunkSize))
                        continue;

                    // Cheap filter: module handle at +0x10 and vkGetInstanceProcAddr at +0x18.
                    for (std::size_t offset = 0; offset + 0x20 <= chunkSize; offset += sizeof(uintptr_t))
                    {
                        uintptr_t module = 0;
                        std::memcpy(&module, buffer.data() + offset + 0x10, sizeof(module));
                        if (module != moduleValue)
                            continue;
                        uintptr_t proc = 0;
                        std::memcpy(&proc, buffer.data() + offset + 0x18, sizeof(proc));
                        if (proc != procValue)
                            continue;

                        const uintptr_t candidate = chunkBase + offset;
                        if (LooksLikeVulkanDeviceTable(candidate, moduleValue, procValue))
                        {
                            uintptr_t expected = 0;
                            if (g_vulkanDeviceTable.compare_exchange_strong(expected, candidate))
                            {
                                std::ostringstream oss;
                                oss << "[Minimap] found existing Vulkan device table at " << Hex(candidate);
                                Log(oss.str());
                            }
                            return true;
                        }
                    }

                    if (chunkSize < CHUNK)
                        break;
                }
            }

            if (next <= cursor)
                break;
            cursor = next;
        }

        return false;
    }

    std::atomic<bool> g_vulkanTableScanBusy{ false };

    // Non-blocking: starts a background scan unless one is running or the table is known.
    bool TryFindExistingVulkanDeviceTable()
    {
        if (g_vulkanDeviceTable.load() != 0)
            return true;

        if (g_vulkanTableScanBusy.exchange(true))
            return false;

        std::thread([]()
        {
            BackgroundThreadScope scope;
            const DWORD start = GetTickCount();
            const bool found = ScanForVulkanDeviceTable();
            if (g_debugLoggingEnabled.load())
            {
                std::ostringstream oss;
                oss << "[Minimap] Vulkan device table scan finished | found=" << (found ? "yes" : "no")
                    << " | ms=" << (GetTickCount() - start);
                Log(oss.str());
            }
            g_vulkanTableScanBusy.store(false);
        }).detach();
        return false;
    }

    void ProbeVulkanTable()
    {
        if (g_vulkanDeviceTable.load() == 0)
        {
            const DWORD now = GetTickCount();
            if (g_vulkanScanAttempts < 3 && now - g_lastVulkanScanTick >= 10000)
            {
                g_lastVulkanScanTick = now;
                ++g_vulkanScanAttempts;
                TryFindExistingVulkanDeviceTable();
            }
        }

        const uintptr_t table = g_vulkanDeviceTable.load();
        if (table == 0)
            return;

        TryInstallVulkanTableHooks(table);

        uintptr_t queueSubmit = 0;
        uintptr_t createSwapchain = 0;
        uintptr_t getSwapchainImages = 0;
        uintptr_t acquireNextImage = 0;
        uintptr_t queuePresent = 0;

        if (!ReadVulkanTableFunction(table, VULKAN_TABLE_QUEUE_SUBMIT_OFFSET, queueSubmit) ||
            !ReadVulkanTableFunction(table, VULKAN_TABLE_CREATE_SWAPCHAIN_OFFSET, createSwapchain) ||
            !ReadVulkanTableFunction(table, VULKAN_TABLE_GET_SWAPCHAIN_IMAGES_OFFSET, getSwapchainImages) ||
            !ReadVulkanTableFunction(table, VULKAN_TABLE_ACQUIRE_NEXT_IMAGE_OFFSET, acquireNextImage) ||
            !ReadVulkanTableFunction(table, VULKAN_TABLE_QUEUE_PRESENT_OFFSET, queuePresent))
        {
            return;
        }

        if (!g_debugLoggingEnabled.load())
            return;

        const DWORD now = GetTickCount();
        if (now - g_lastVulkanSummaryTick < 5000)
            return;

        g_lastVulkanSummaryTick = now;

        std::ostringstream oss;
        oss << "[Minimap] Vulkan table"
            << " | table=" << Hex(table)
            << " | queueSubmit=" << Hex(queueSubmit)
            << " | createSwapchain=" << Hex(createSwapchain)
            << " | getSwapchainImages=" << Hex(getSwapchainImages)
            << " | acquireNextImage=" << Hex(acquireNextImage)
            << " | queuePresent=" << Hex(queuePresent);
        Log(oss.str());
    }

    void __fastcall CaptureVulkanDeviceTableInitHook(void* table, void*, void*, void*)
    {
        if (table == nullptr)
            return;

        const uintptr_t previous = g_vulkanDeviceTable.exchange(reinterpret_cast<uintptr_t>(table));
        TryInstallVulkanTableHooks(reinterpret_cast<uintptr_t>(table));
        if (previous != reinterpret_cast<uintptr_t>(table))
        {
            std::ostringstream oss;
            oss << "[Minimap] captured Vulkan device table candidate at "
                << Hex(reinterpret_cast<uintptr_t>(table));
            Log(oss.str());
        }
    }

    class MinimapMod : public Mod
    {
    public:
        void Load(ModContext* modContext) override
        {
            if (!modContext->game.isClient)
            {
                modContext->Log("[Minimap] client-only mod; skipping load on server");
                return;
            }

            g_modContext = modContext;
            g_shroudtopiaConfigPath = ResolveShroudtopiaConfigPath(modContext);
            RefreshMinimapConfig(modContext, true);
            g_exeBase = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));
            g_exeImageSize = GetImageSize(g_exeBase);

            const uintptr_t uiRenderSetupRva = ResolvePatternRvaNear(
                RVA_LOCAL_PLAYER_UI_RENDER_SETUP,
                g_localPlayerUiRenderSetupExpected.data(),
                g_localPlayerUiRenderSetupExpected.size(),
                "local_player_ui_render_setup"
            );
            const uintptr_t waypointsUiRva = ResolvePatternRvaNear(
                RVA_PLAYER_WAYPOINTS_UI,
                g_playerWaypointsUiExpected.data(),
                g_playerWaypointsUiExpected.size(),
                "player_waypoints_ui"
            );
            const uintptr_t mapMarkerVisibilityRva = ResolvePatternRvaNear(
                RVA_KNOWLEDGE_QUERY_MAPMARKER_VISIBILITY,
                g_mapMarkerVisibilityExpected.data(),
                g_mapMarkerVisibilityExpected.size(),
                "map_marker_visibility"
            );
            const uintptr_t mapMarkerVisibilityLoopRva = mapMarkerVisibilityRva != 0
                ? mapMarkerVisibilityRva + (RVA_KNOWLEDGE_QUERY_MAPMARKER_VISIBILITY_LOOP - RVA_KNOWLEDGE_QUERY_MAPMARKER_VISIBILITY)
                : 0;
            const uintptr_t fogOfWarRva = ResolvePatternRvaNear(
                RVA_FOG_OF_WAR,
                g_fogOfWarExpected.data(),
                g_fogOfWarExpected.size(),
                "fog_of_war"
            );
            const uintptr_t renderPresentFrameRva = ResolvePatternRvaNear(
                RVA_RENDER_PRESENT_FRAME,
                g_renderPresentFrameExpected.data(),
                g_renderPresentFrameExpected.size(),
                "render_present_frame"
            );
            const uintptr_t vulkanDeviceTableInitRva = ResolvePatternRvaNear(
                RVA_VULKAN_DEVICE_TABLE_INIT,
                g_vulkanDeviceTableInitExpected.data(),
                g_vulkanDeviceTableInitExpected.size(),
                "vulkan_device_table_init"
            );

            uintptr_t iterInitRva = 0;
            uintptr_t iterNextRva = 0;
            if (!TryFindRelativeCallTargetRva(uiRenderSetupRva, 0x60, 1, iterInitRva))
                TryFindRelativeCallTargetRva(mapMarkerVisibilityRva, 0x60, 1, iterInitRva);
            TryFindRelativeCallTargetRva(mapMarkerVisibilityRva, 0x60, 2, iterNextRva);

            if (iterInitRva != 0)
            {
                g_iterInit = reinterpret_cast<IterInitFn>(g_exeBase + iterInitRva);
                if (iterInitRva != RVA_ITER_INIT)
                {
                    std::ostringstream oss;
                    oss << "[Minimap] iterator init resolved"
                        << " | preferred=" << Hex(RVA_ITER_INIT)
                        << " | resolved=" << Hex(iterInitRva);
                    modContext->Log(oss.str().c_str());
                }
            }
            else
            {
                g_iterInit = nullptr;
                modContext->Log("[Minimap] iterator init unavailable");
            }

            if (iterNextRva != 0)
            {
                g_iterNext = reinterpret_cast<IterNextFn>(g_exeBase + iterNextRva);
                if (iterNextRva != RVA_ITER_NEXT)
                {
                    std::ostringstream oss;
                    oss << "[Minimap] iterator next resolved"
                        << " | preferred=" << Hex(RVA_ITER_NEXT)
                        << " | resolved=" << Hex(iterNextRva);
                    modContext->Log(oss.str().c_str());
                }
            }
            else
            {
                g_iterNext = nullptr;
                modContext->Log("[Minimap] iterator next unavailable");
            }

            g_localPlayerUiRenderSetupHook = InstallEntryHook(
                uiRenderSetupRva,
                g_localPlayerUiRenderSetupExpected,
                reinterpret_cast<void*>(&CaptureUiRenderSetupHook),
                "local_player_ui_render_setup"
            );

            g_playerWaypointsUiHook = InstallEntryHook(
                waypointsUiRva,
                g_playerWaypointsUiExpected,
                reinterpret_cast<void*>(&CaptureWaypointsHook),
                "player_waypoints_ui"
            );

            g_mapMarkerVisibilityHook = MARKER_VISIBILITY_HOOK_ENABLED
                ? InstallMapMarkerVisibilityLoopHook(
                    mapMarkerVisibilityLoopRva,
                    reinterpret_cast<void*>(&CaptureMapMarkerVisibilityRecordHook))
                : nullptr;

            g_fogOfWarHook = InstallEntryHook(
                fogOfWarRva,
                g_fogOfWarExpected,
                reinterpret_cast<void*>(&CaptureFogOfWarHook),
                "fog_of_war"
            );

            g_renderPresentFrameHook = InstallEntryHook(
                renderPresentFrameRva,
                g_renderPresentFrameExpected,
                reinterpret_cast<void*>(&CaptureRenderPresentFrameHook),
                "render_present_frame"
            );

            g_vulkanDeviceTableInitHook = InstallEntryHook(
                vulkanDeviceTableInitRva,
                g_vulkanDeviceTableInitExpected,
                reinterpret_cast<void*>(&CaptureVulkanDeviceTableInitHook),
                "vulkan_device_table_init"
            );

            std::ostringstream oss;
            oss << "[Minimap] load complete | mode=internal_data_bridge"
                << " | external_overlay=disabled"
                << " | ui_hook=" << (g_localPlayerUiRenderSetupHook != nullptr ? "ready" : "missing")
                << " | waypoint_hook=" << (g_playerWaypointsUiHook != nullptr ? "ready" : "missing")
                << " | marker_visibility_hook=" << (g_mapMarkerVisibilityHook != nullptr ? "ready" : "missing")
                << " | fog_hook=" << (g_fogOfWarHook != nullptr ? "ready" : "missing")
                << " | render_hook=" << (g_renderPresentFrameHook != nullptr ? "ready" : "missing")
                << " | vulkan_probe=" << (g_vulkanDeviceTableInitHook != nullptr ? "ready" : "missing");
            modContext->Log(oss.str().c_str());
            loaded = true;
        }

        void Unload(ModContext* modContext) override
        {
            // Background workers read game memory through this DLL's code: they must be
            // gone before anything is torn down. Every loop checks MinimapRuntimeActive().
            g_shuttingDown.store(true);
            g_minimapEnabled.store(false);
            for (int waited = 0; waited < 100 && g_backgroundThreads.load() > 0; ++waited)
                Sleep(20);

            {
                std::lock_guard<std::mutex> lock(g_rendererMutex);
                DestroyVulkanMinimapRendererLocked();
            }
            RestoreVulkanTableHooks();

            if (g_localPlayerUiRenderSetupHook != nullptr)
            {
                g_localPlayerUiRenderSetupHook->deactivate();
                delete g_localPlayerUiRenderSetupHook;
                g_localPlayerUiRenderSetupHook = nullptr;
            }

            if (g_playerWaypointsUiHook != nullptr)
            {
                g_playerWaypointsUiHook->deactivate();
                delete g_playerWaypointsUiHook;
                g_playerWaypointsUiHook = nullptr;
            }

            if (g_mapMarkerVisibilityHook != nullptr)
            {
                g_mapMarkerVisibilityHook->deactivate();
                delete g_mapMarkerVisibilityHook;
                g_mapMarkerVisibilityHook = nullptr;
            }

            if (g_fogOfWarHook != nullptr)
            {
                g_fogOfWarHook->deactivate();
                delete g_fogOfWarHook;
                g_fogOfWarHook = nullptr;
            }

            if (g_renderPresentFrameHook != nullptr)
            {
                g_renderPresentFrameHook->deactivate();
                delete g_renderPresentFrameHook;
                g_renderPresentFrameHook = nullptr;
            }

            if (g_vulkanDeviceTableInitHook != nullptr)
            {
                g_vulkanDeviceTableInitHook->deactivate();
                delete g_vulkanDeviceTableInitHook;
                g_vulkanDeviceTableInitHook = nullptr;
            }

            {
                std::lock_guard<std::mutex> lock(g_waypointMutex);
                g_waypoints.clear();
            }
            {
                std::lock_guard<std::mutex> lock(g_nearbyMarkerMutex);
                g_nearbyMarkers.clear();
            }
            {
                std::lock_guard<std::mutex> lock(g_visibleMapMarkerMutex);
                g_visibleMapMarkers.clear();
            }
            {
                std::lock_guard<std::mutex> lock(g_playerPositionMutex);
                g_playerPosition = {};
            }
            {
                std::lock_guard<std::mutex> lock(g_swapchainMutex);
                g_swapchains.clear();
            }

            g_worldSessionReady.store(false);
            g_gameSessionOnline.store(false);
            g_lastWorldDataTick.store(0);
            g_gameLogPath.clear();
            g_shroudtopiaConfigPath.clear();
            g_lastConfigPollTick = 0;
            g_lastSessionLogPollTick = 0;
            g_vulkanDeviceTable.store(0);
            g_lastRenderContext.store(0);
            g_lastVulkanDevice.store(0);
            g_lastGraphicsContext.store(0);
            g_lastSwapchainState.store(0);
            g_lastVulkanQueueFamilyIndex.store(0);
            g_vulkanScanAttempts = 0;
            g_lastVulkanScanTick = 0;
            g_lastVulkanFunctionScanTick = 0;
            g_vulkanFunctionOffsetsLogged = false;
            g_iterInit = nullptr;
            g_iterNext = nullptr;
            g_modContext = nullptr;
            active = false;
            loaded = false;
            modContext->Log("[Minimap] unloaded");
        }

        void Activate(ModContext* modContext) override
        {
            RefreshMinimapConfig(modContext, true);
            const bool uiOk = ActivateHook(g_localPlayerUiRenderSetupHook);
            const bool waypointOk = ActivateHook(g_playerWaypointsUiHook);
            const bool markerVisibilityOk = ActivateHook(g_mapMarkerVisibilityHook);
            const bool fogOk = ActivateHook(g_fogOfWarHook);
            const bool renderOk = ActivateHook(g_renderPresentFrameHook);
            const bool vulkanOk = ActivateHook(g_vulkanDeviceTableInitHook);
            active = uiOk || waypointOk || markerVisibilityOk || renderOk || vulkanOk;
            if (active)
                TryFindExistingVulkanDeviceTable();

            std::ostringstream oss;
            oss << "[Minimap] activate internal bridge"
                << " | ui_hook=" << HookActivationState(g_localPlayerUiRenderSetupHook)
                << " | waypoint_hook=" << HookActivationState(g_playerWaypointsUiHook)
                << " | marker_visibility_hook=" << HookActivationState(g_mapMarkerVisibilityHook)
                << " | fog_hook=" << (fogOk ? "active" : HookActivationState(g_fogOfWarHook))
                << " | render_hook=" << HookActivationState(g_renderPresentFrameHook)
                << " | vulkan_probe=" << HookActivationState(g_vulkanDeviceTableInitHook)
                << " | external_overlay=disabled";
            modContext->Log(oss.str().c_str());
        }

        void Deactivate(ModContext* modContext) override
        {
            {
                std::lock_guard<std::mutex> lock(g_rendererMutex);
                DestroyVulkanMinimapRendererLocked();
            }

            if (g_localPlayerUiRenderSetupHook != nullptr)
                g_localPlayerUiRenderSetupHook->deactivate();

            if (g_playerWaypointsUiHook != nullptr)
                g_playerWaypointsUiHook->deactivate();

            if (g_mapMarkerVisibilityHook != nullptr)
                g_mapMarkerVisibilityHook->deactivate();

            if (g_fogOfWarHook != nullptr)
                g_fogOfWarHook->deactivate();

            if (g_renderPresentFrameHook != nullptr)
                g_renderPresentFrameHook->deactivate();

            if (g_vulkanDeviceTableInitHook != nullptr)
                g_vulkanDeviceTableInitHook->deactivate();

            g_worldSessionReady.store(false);
            g_gameSessionOnline.store(false);
            g_lastWorldDataTick.store(0);
            g_lastConfigPollTick = 0;
            {
                std::lock_guard<std::mutex> lock(g_visibleMapMarkerMutex);
                g_visibleMapMarkers.clear();
            }
            {
                std::lock_guard<std::mutex> lock(g_playerPositionMutex);
                g_playerPosition = {};
            }
            active = false;
            modContext->Log("[Minimap] deactivated");
        }

        void Update(ModContext* modContext) override
        {
            if (active)
            {
                UpdateMinimapRuntimeControls(modContext, true);
                PollGameSessionLog();
                ProbeVulkanTable();
                MaybeStartLivePositionTracker();
            }
        }

        ModMetaData GetMetaData() override
        {
            return g_metaData;
        }
    };
}

extern "C" __declspec(dllexport) Mod* CreateModInstance()
{
    return new MinimapMod();
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID)
{
    return TRUE;
}
