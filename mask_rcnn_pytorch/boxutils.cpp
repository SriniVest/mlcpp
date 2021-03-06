#include "boxutils.h"

torch::Tensor BBoxOverlaps(torch::Tensor boxes1, torch::Tensor boxes2) {
  // 1. Tile boxes2 and repeate boxes1. This allows us to compare
  // every boxes1 against every boxes2 without loops.

  auto boxes1_repeat = boxes2.size(0);
  auto boxes2_repeat = boxes1.size(0);
  boxes1 = boxes1.repeat({1, boxes1_repeat}).view({-1, 4});
  boxes2 = boxes2.repeat({boxes2_repeat, 1});

  // 2. Compute intersections
  auto b1 = boxes1.chunk(4, /*dim*/ 1);
  auto b2 = boxes2.chunk(4, /*dim*/ 1);
  auto y1 = torch::max(b1[0], b2[0]).narrow(1, 0, 1);
  auto x1 = torch::max(b1[1], b2[1]).narrow(1, 0, 1);
  auto y2 = torch::min(b1[2], b2[2]).narrow(1, 0, 1);
  auto x2 = torch::min(b1[3], b2[3]).narrow(1, 0, 1);
  auto zeros = torch::zeros({y1.size(0), 1}, at::requires_grad(false));
  if (y1.is_cuda())
    zeros = zeros.cuda();
  auto intersection =
      torch::mul(torch::max(x2 - x1, zeros), torch::max(y2 - y1, zeros));

  // 3. Compute unions
  auto b1_area = (b1[2] - b1[1]) * (b1[3] - b1[0]);
  auto b2_area = (b2[2] - b2[1]) * (b2[3] - b2[0]);
  auto box_union =
      b1_area.narrow(1, 0, 1) + b2_area.narrow(1, 0, 1) - intersection;

  // 4. Compute IoU and reshape to [boxes1, boxes2]
  auto iou = intersection / box_union;
  auto overlaps = iou.view({boxes2_repeat, boxes1_repeat});

  return overlaps;
}

/* Calculates IoU of the given box with the array of the given boxes.
 * box: 1D vector [y1, x1, y2, x2]
 * boxes: [boxes_count, (y1, x1, y2, x2)]
 * box_area: float. the area of 'box'
 * boxes_area: array of length boxes_count.
 * Note: the areas are passed in rather than calculated here for
 *       efficency. Calculate once in the caller to avoid duplicate work.
 */
static at::Tensor ComputeIou(at::Tensor box,
                             at::Tensor boxes,
                             at::Tensor box_area,
                             at::Tensor boxes_area) {
  auto y1 = torch::max(box[0], boxes.narrow(1, 0, 1));
  auto y2 = torch::min(box[2], boxes.narrow(1, 2, 1));
  auto x1 = torch::max(box[1], boxes.narrow(1, 1, 1));
  auto x2 = torch::min(box[3], boxes.narrow(1, 3, 1));
  auto intersection = torch::max(x2 - x1, torch::tensor(0.f)) *
                      torch::max(y2 - y1, torch::tensor(0.f));
  auto union_ = box_area + boxes_area - intersection;
  auto iou = intersection / union_;
  return iou;
}

torch::Tensor BBoxOverlapsLoops(torch::Tensor boxes1, torch::Tensor boxes2) {
  // Areas of anchors and GT boxes
  auto area1 = (boxes1.narrow(1, 2, 1) - boxes1.narrow(1, 0, 1)) *
               (boxes1.narrow(1, 3, 1) - boxes1.narrow(1, 1, 1));
  auto area2 = (boxes2.narrow(1, 2, 1) - boxes2.narrow(1, 0, 1)) *
               (boxes2.narrow(1, 3, 1) - boxes2.narrow(1, 1, 1));

  // Compute overlaps to generate matrix [boxes1 count, boxes2 count]
  // Each cell contains the IoU value.
  auto overlaps = torch::zeros({boxes1.size(0), boxes2.size(0)});
  for (int64_t i = 0; i < overlaps.size(1); ++i) {
    auto box2 = boxes2[i];
    auto iou = ComputeIou(box2, boxes1, area2[i], area1);
    overlaps.narrow(1, i, 1) = iou;
  }
  return overlaps;
}

torch::Tensor BoxRefinement(torch::Tensor box, torch::Tensor gt_box) {
  auto height = box.narrow(1, 2, 1) - box.narrow(1, 0, 1);
  auto width = box.narrow(1, 3, 1) - box.narrow(1, 1, 1);
  auto center_y = box.narrow(1, 0, 1) + 0.5f * height;
  auto center_x = box.narrow(1, 1, 1) + 0.5f * width;

  auto gt_height = gt_box.narrow(1, 2, 1) - gt_box.narrow(1, 0, 1);
  auto gt_width = gt_box.narrow(1, 3, 1) - gt_box.narrow(1, 1, 1);
  auto gt_center_y = gt_box.narrow(1, 0, 1) + 0.5f * gt_height;
  auto gt_center_x = gt_box.narrow(1, 1, 1) + 0.5f * gt_width;

  auto dy = (gt_center_y - center_y) / height;
  auto dx = (gt_center_x - center_x) / width;
  auto dh = torch::log(gt_height / height);
  auto dw = torch::log(gt_width / width);

  auto result = torch::stack(
      {dy.squeeze(1), dx.squeeze(1), dh.squeeze(1), dw.squeeze(1)}, /*dim*/ 1);
  return result;
}

at::Tensor ApplyBoxDeltas(at::Tensor boxes, at::Tensor deltas) {
  // Convert to y, x, h, w
  auto height = boxes.narrow(1, 2, 1) - boxes.narrow(1, 0, 1);
  auto width = boxes.narrow(1, 3, 1) - boxes.narrow(1, 1, 1);
  auto center_y = boxes.narrow(1, 0, 1) + 0.5f * height;
  auto center_x = boxes.narrow(1, 1, 1) + 0.5f * width;
  // Apply deltas
  center_y += deltas.narrow(1, 0, 1) * height;
  center_x += deltas.narrow(1, 1, 1) * width;

  height *= torch::exp(deltas.narrow(1, 2, 1));
  width *= torch::exp(deltas.narrow(1, 3, 1));
  // Convert back to y1, x1, y2, x2
  auto y1 = center_y - 0.5f * height;
  auto x1 = center_x - 0.5f * width;
  auto y2 = y1 + height;
  auto x2 = x1 + width;

  auto result = torch::stack(
      {y1.squeeze(), x1.squeeze(), y2.squeeze(), x2.squeeze()}, /*dim*/ 1);
  return result;
}

/*
 * boxes: [N, 4] each col is y1, x1, y2, x2
 * window: [4] in the form y1, x1, y2, x2
 */
at::Tensor ClipBoxes(at::Tensor boxes, Window window) {
  boxes = torch::stack(
      {boxes.narrow(1, 0, 1)
           .clamp(static_cast<float>(window.y1), static_cast<float>(window.y2))
           .squeeze(),
       boxes.narrow(1, 1, 1)
           .clamp(static_cast<float>(window.x1), static_cast<float>(window.x2))
           .squeeze(),
       boxes.narrow(1, 2, 1)
           .clamp(static_cast<float>(window.y1), static_cast<float>(window.y2))
           .squeeze(),
       boxes.narrow(1, 3, 1)
           .clamp(static_cast<float>(window.x1), static_cast<float>(window.x2))
           .squeeze()},
      1);
  return boxes;
}

at::Tensor ClipToWindow(const Window& window, at::Tensor boxes) {
  boxes.narrow(1, 0, 1) = boxes.narrow(1, 0, 1).clamp(
      static_cast<float>(window.y1), static_cast<float>(window.y2));
  boxes.narrow(1, 1, 1) = boxes.narrow(1, 1, 1).clamp(
      static_cast<float>(window.x1), static_cast<float>(window.x2));
  boxes.narrow(1, 2, 1) = boxes.narrow(1, 2, 1).clamp(
      static_cast<float>(window.y1), static_cast<float>(window.y2));
  boxes.narrow(1, 3, 1) = boxes.narrow(1, 3, 1).clamp(
      static_cast<float>(window.x1), static_cast<float>(window.x2));
  return boxes;
}
