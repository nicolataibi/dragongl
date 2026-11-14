/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * vk_frame.c - One frame: acquire, record, submit, present (vk_frame.h).
 */

#include "vk_frame.h"

#include "client_fct.h"
#include "client_input.h"
#include "client_math.h"
#include "client_particles.h"
#include "client_state.h"
#include "hud.h"
#include "vk_scene.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/*What the passes of a frame need to know about it.*/
typedef struct {
    const float *mvp;
    const float *hud_ortho;
    float vision_radius;
    float px, pz;
    float time;
} FrameView;

/*The colour image leaves UNDEFINED for rendering. The depth image is
 * shared by every frame in flight: its barrier also orders this frame's
 * clear after the previous frame's depth writes (write-after-write), and
 * moves it out of UNDEFINED (it is cleared at load, so the old contents
 * never matter).*/
static void begin_attachments(VkState *s, VkFrameResources *frame) {
    VkImageMemoryBarrier2 color_barrier = {0};
    color_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    color_barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    color_barrier.srcAccessMask = 0;
    color_barrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    color_barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    color_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color_barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    color_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    color_barrier.image = s->images[s->current_image];
    color_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    color_barrier.subresourceRange.baseMipLevel = 0;
    color_barrier.subresourceRange.levelCount = 1;
    color_barrier.subresourceRange.baseArrayLayer = 0;
    color_barrier.subresourceRange.layerCount = 1;

    VkImageMemoryBarrier2 depth_barrier = {0};
    depth_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    depth_barrier.srcStageMask =
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    depth_barrier.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    depth_barrier.dstStageMask =
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    depth_barrier.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                  VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    depth_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth_barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depth_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depth_barrier.image = s->depth_image;
    depth_barrier.subresourceRange.aspectMask = s->depth_aspect;
    depth_barrier.subresourceRange.levelCount = 1;
    depth_barrier.subresourceRange.layerCount = 1;

    VkImageMemoryBarrier2 attach_barriers[2] = {color_barrier, depth_barrier};
    VkDependencyInfo dep_info = {0};
    dep_info.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep_info.imageMemoryBarrierCount = 2;
    dep_info.pImageMemoryBarriers = attach_barriers;

    vkCmdPipelineBarrier2(frame->command_buffer, &dep_info);
}

/*A compute stage wrote what the next one reads.*/
static void compute_barrier(VkCommandBuffer cb, VkPipelineStageFlags2 src_stage,
                            VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                            VkAccessFlags2 dst_access) {
    VkMemoryBarrier2 barrier = {0};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    VkDependencyInfo dep = {0};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cb, &dep);
}

/*One compute dispatch over `count` instances with its descriptor set;
 * the push constants only where the shader reads the count.*/
static void dispatch(VkCommandBuffer cb, VkPipeline pipe, VkPipelineLayout layout,
                     const VkDescriptorSet *set, uint32_t count, GpdPush *pc) {
    uint32_t groups = (count + VKD_WORKGROUP - 1) / VKD_WORKGROUP;
    if (groups == 0) {
        return;
    }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, set, 0, NULL);
    if (pc) {
        pc->count = count;
        vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GpdPush), pc);
    }
    vkCmdDispatch(cb, groups, 1, 1);
}

/*The GPU-driven scene (shaders/gpd_*.comp): reset the counters, cull the
 * instances around the player, expand the survivors into vertices, write
 * the indirect draw.*/
static void compute_scene(VkState *s, VkFrameResources *frame, const FrameView *view) {
    VkCommandBuffer cb = frame->command_buffer;
    const VkAccessFlags2 read_write = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
    vkCmdFillBuffer(cb, frame->gpd_counts, 0, 16, 0);
    compute_barrier(cb, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, read_write);

    GpdPush pc;
    pc.cull_radius = view->vision_radius;
    pc.player_x = view->px;
    pc.player_z = view->pz;
    pc.time = view->time;
    pc.capacity = VKD_VERTEX_CAPACITY;
    pc.count = 0;

    dispatch(cb, s->gpd_pipe_cull_map, s->gpd_pl_cull, &frame->gpd_desc_cull_map,
             frame->gpd_map_count, &pc);
    dispatch(cb, s->gpd_pipe_cull_dyn, s->gpd_pl_cull, &frame->gpd_desc_cull_dyn,
             frame->gpd_dyn_count, &pc);
    compute_barrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, read_write);

    dispatch(cb, s->gpd_pipe_exp_map, s->gpd_pl_exp, &frame->gpd_desc_exp_map, frame->gpd_map_count,
             NULL);
    dispatch(cb, s->gpd_pipe_exp_dyn, s->gpd_pl_exp, &frame->gpd_desc_exp_dyn, frame->gpd_dyn_count,
             NULL);
    dispatch(cb, s->gpd_pipe_exp_ori, s->gpd_pl_exp_ori, &frame->gpd_desc_exp_ori,
             frame->gpd_ori_count, &pc);
    compute_barrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, read_write);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s->gpd_pipe_final);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s->gpd_pl_final, 0, 1,
                            &frame->gpd_desc_final, 0, NULL);
    vkCmdDispatch(cb, 1, 1, 1);
    compute_barrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                    VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT);
}

/*The render pass: the 3D scene from the generated vertices, then the HUD
 * on top (no depth test, blended).*/
static void draw_scene_and_hud(VkState *s, VkFrameResources *frame, const FrameView *view) {
    VkRenderingAttachmentInfo colorAttachment = {0};
    colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachment.imageView = s->image_views[s->current_image];
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.clearValue.color.float32[0] = 0.05f;
    colorAttachment.clearValue.color.float32[1] = 0.05f;
    colorAttachment.clearValue.color.float32[2] = 0.08f;
    colorAttachment.clearValue.color.float32[3] = 1.0f;

    VkRenderingAttachmentInfo depthAttachment = {0};
    depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depthAttachment.imageView = s->depth_image_view;
    depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.clearValue.depthStencil.depth = 1.0f;
    depthAttachment.clearValue.depthStencil.stencil = 0;

    VkRenderingInfo renderingInfo = {0};
    renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    renderingInfo.renderArea.offset.x = 0;
    renderingInfo.renderArea.offset.y = 0;
    renderingInfo.renderArea.extent = s->swapchain_extent;
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments = &colorAttachment;
    renderingInfo.pDepthAttachment = &depthAttachment;

    vkCmdBeginRendering(frame->command_buffer, &renderingInfo);

    /*Viewport and scissor are dynamic state: the pipelines do not depend
     *on the window size.*/
    VkViewport viewport = {
        0.0f, 0.0f, (float)s->swapchain_extent.width, (float)s->swapchain_extent.height,
        0.0f, 1.0f};
    VkRect2D scissor = {{0, 0}, s->swapchain_extent};
    vkCmdSetViewport(frame->command_buffer, 0, 1, &viewport);
    vkCmdSetScissor(frame->command_buffer, 0, 1, &scissor);

    /* --- Step 1: 3D Scene --- */
    float push_data[20];
    memcpy(push_data, view->mvp, sizeof(float) * 16);
    push_data[16] = view->vision_radius;
    push_data[17] = view->px;
    push_data[18] = view->pz;
    push_data[19] = view->time;

    vkCmdBindPipeline(frame->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      s->gpd_scene_pipeline);
    vkCmdBindDescriptorSets(frame->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            s->gpd_scene_layout, 0, 1, &frame->gpd_desc_scene, 0, NULL);
    vkCmdPushConstants(frame->command_buffer, s->gpd_scene_layout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(float) * 20, push_data);
    vkCmdDrawIndirect(frame->command_buffer, frame->gpd_indirect, 0, 1, 16);

    /*--- Step 2: 2D HUD overlay (pipeline without depth test, with blend) ---*/
    if (s->hud_vertex_count > 0 && s->pipeline_hud != VK_NULL_HANDLE) {
        vkCmdBindPipeline(frame->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipeline_hud);
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(frame->command_buffer, 0, 1, &frame->vertex_buffer, offsets);
        float hud_push[17];
        memcpy(hud_push, view->hud_ortho, sizeof(float) * 16);
        hud_push[16] = 99999.0f; /* disables fog in the fragment shader */
        vkCmdPushConstants(frame->command_buffer, s->pipeline_layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(float) * 17, hud_push);
        vkCmdDraw(frame->command_buffer, s->hud_vertex_count, 1, 0, 0);
    }

    vkCmdEndRendering(frame->command_buffer);
}

/*The colour image goes to the presentation layout.*/
static void end_attachments(VkState *s, VkFrameResources *frame) {
    VkImageMemoryBarrier2 present_barrier = {0};
    present_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    present_barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    present_barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    present_barrier.dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
    present_barrier.dstAccessMask = 0;
    present_barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    present_barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    present_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    present_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    present_barrier.image = s->images[s->current_image];
    present_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    present_barrier.subresourceRange.baseMipLevel = 0;
    present_barrier.subresourceRange.levelCount = 1;
    present_barrier.subresourceRange.baseArrayLayer = 0;
    present_barrier.subresourceRange.layerCount = 1;

    VkDependencyInfo present_dep_info = {0};
    present_dep_info.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    present_dep_info.imageMemoryBarrierCount = 1;
    present_dep_info.pImageMemoryBarriers = &present_barrier;

    vkCmdPipelineBarrier2(frame->command_buffer, &present_dep_info);
}

static void record_commands(VkState *s, VkFrameResources *frame, const FrameView *view) {
    VkCommandBufferBeginInfo begin = {0};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(frame->command_buffer, &begin);
    begin_attachments(s, frame);
    compute_scene(s, frame, view);
    draw_scene_and_hud(s, frame, view);
    end_attachments(s, frame);
    vkEndCommandBuffer(frame->command_buffer);
}

/*The camera orbiting the player's tile: model-view-projection of the
 * scene.*/
static void camera_matrix(float mvp[16], float px, float pz, float aspect) {
    const Camera *cam = client_camera();
    float pitch = cam->pitch * (float)M_PI / 180.0f;
    float yaw = cam->yaw * (float)M_PI / 180.0f;
    float center[3] = {px, 0.0f, pz};
    float eye[3] = {px + cam->dist * cosf(pitch) * sinf(yaw), cam->dist * sinf(pitch),
                    pz + cam->dist * cosf(pitch) * cosf(yaw)};
    float up[3] = {0.0f, 1.0f, 0.0f};
    float view[16], proj[16];
    mat4_lookat(view, eye, center, up);
    mat4_perspective(proj, 45.0f * (float)M_PI / 180.0f, aspect, 0.5f, 500.0f);
    mat4_mul(mvp, proj, view);
}

/*Steps 1 and 2 of a frame: wait for the slot, acquire an image. False
 * when there is nothing to draw this time (swapchain out of date, or the
 * device lost).*/
static bool acquire_image(VkState *s, uint32_t slot, uint32_t *image) {
    vkWaitForFences(s->device, 1, &s->fences[slot], VK_TRUE, UINT64_MAX);
    VkResult res = vkAcquireNextImageKHR(s->device, s->swapchain, UINT64_MAX, s->sem_image[slot],
                                         VK_NULL_HANDLE, image);
    if (res == VK_ERROR_OUT_OF_DATE_KHR) {
        s->swapchain_dirty = true;
        return false;
    }
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) {
        fprintf(stderr, "[VK] vkAcquireNextImageKHR failed: %d\n", res);
        s->device_lost = true;
        return false;
    }
    if (res == VK_SUBOPTIMAL_KHR) {
        s->swapchain_dirty = true; /* this frame is still presentable */
    }
    return true;
}

/*The instances of the scene and the vertices of the HUD for this frame.*/
static void fill_frame(VkState *s, VkFrameResources *frame, const FrameSnapshot *snap,
                       const float mvp[16], float dt, float frame_ms) {
    const ClientUi *ui = client_ui();
    float px = (snap->my_x != -1) ? (float)snap->my_x : 500.0f;
    float pz = (snap->my_y != -1) ? (float)snap->my_y : 500.0f;

    /*Advance the particle simulation once per rendered frame, right
     *before the dynamic instances are filled from the pool. The pool is
     *shared with the net thread (spawn_vfx on MSG_SPELL_VFX) and guarded
     *by g_particles_mutex (client_particles.h).*/
    particles_update(dt);
    vk_scene_update(s, dt, snap, !ui->reduce_motion);
    fct_update(dt);

    /*2D HUD: the slot's host-visible vertex buffer (persistently mapped);
     * the 3D scene is generated on the GPU.*/
    Hud hud;
    hud_begin(&hud, (VkVertex *)frame->mapped_vertex_data, s->max_vertices,
              (float)s->swapchain_extent.width, (float)s->swapchain_extent.height, ui->scale,
              s->swapchain_format == VK_FORMAT_B8G8R8A8_SRGB ||
                  s->swapchain_format == VK_FORMAT_R8G8B8A8_SRGB);
    hud_world_labels(&hud, mvp, snap, px, pz, ui->reduce_motion);
    pthread_mutex_lock(&g_state_mutex);
    hud_panels(&hud, ui, client_input_config(), frame_ms);
    pthread_mutex_unlock(&g_state_mutex);
    s->hud_vertex_count = hud.count;
}

/*Steps 3 and 4: submit (waits on the image semaphore, signals the
 * image's render-finished semaphore and the slot's fence) and present.*/
static void submit_and_present(VkState *s, uint32_t slot, uint32_t image) {
    VkFrameResources *frame = &s->frames[slot];
    VkSemaphore render_done = s->sem_render[image];
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit = {0};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &s->sem_image[slot];
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &frame->command_buffer;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &render_done;
    VkResult res = vkQueueSubmit(s->graphics_queue, 1, &submit, s->fences[slot]);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "[VK] vkQueueSubmit failed: %d\n", res);
        s->device_lost = true;
        return;
    }

    VkPresentInfoKHR present = {0};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &render_done;
    present.swapchainCount = 1;
    present.pSwapchains = &s->swapchain;
    present.pImageIndices = &image;
    res = vkQueuePresentKHR(s->present_queue, &present);
    if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR) {
        s->swapchain_dirty = true;
    } else if (res != VK_SUCCESS) {
        fprintf(stderr, "[VK] vkQueuePresentKHR failed: %d\n", res);
        s->device_lost = true;
    }
}

/*One frame, in the order Vulkan requires:
 *  1. wait for the fence of this slot's previous use (MAX_FRAMES_IN_FLIGHT
 *     frames ago): its command buffer, HUD buffer, instance buffers and
 *     its image-available semaphore are free again;
 *  2. acquire a swapchain image, signalling the slot's image semaphore
 *     (OUT_OF_DATE: rebuild the swapchain and skip the frame - the fence
 *     is still signalled, nothing was submitted);
 *  3. reset the fence, record, submit;
 *  4. present, waiting on that image's semaphore. The render-finished
 *     semaphore belongs to the swapchain image, not to the frame slot:
 *     only re-acquiring the same image guarantees its previous present
 *     has consumed it.*/
void vk_draw_frame(VkState *s, float dt, double time, float frame_ms) {
    uint32_t slot = s->current_frame;
    VkFrameResources *frame = &s->frames[slot];
    uint32_t image = 0;
    if (!acquire_image(s, slot, &image)) {
        return;
    }
    vkResetFences(s->device, 1, &s->fences[slot]);
    vkResetCommandBuffer(frame->command_buffer, 0);
    s->current_image = image;

    /*The snapshot is large (the whole map): not on the stack.*/
    static FrameSnapshot snap;
    frame_snapshot_acquire(&snap);
    float sw = (float)s->swapchain_extent.width, sh = (float)s->swapchain_extent.height;
    float mvp[16], hud_ortho[16];
    FrameView view;
    view.px = (snap.my_x != -1) ? (float)snap.my_x : 500.0f;
    view.pz = (snap.my_y != -1) ? (float)snap.my_y : 500.0f;
    view.vision_radius = (float)snap.vision_radius;
    /*The water does not wave with reduce_motion.*/
    view.time = client_ui()->reduce_motion ? 0.0f : (float)time;
    camera_matrix(mvp, view.px, view.pz, sw / sh);
    mat4_ortho(hud_ortho, sw, sh);
    view.mvp = mvp;
    view.hud_ortho = hud_ortho;

    fill_frame(s, frame, &snap, mvp, dt, frame_ms);
    record_commands(s, frame, &view);
    submit_and_present(s, slot, image);
    s->current_frame = (slot + 1) % MAX_FRAMES_IN_FLIGHT;
}
