/* ide-truncate-model.h
 *
 * Copyright 2019 Christian Hergert <chergert@redhat.com>
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
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

#define IDE_TYPE_TRUNCATE_MODEL (ide_truncate_model_get_type())

G_DECLARE_FINAL_TYPE (IdeTruncateModel, ide_truncate_model, IDE, TRUNCATE_MODEL, GObject)

IdeTruncateModel *ide_truncate_model_new             (GListModel       *child_model);
GListModel       *ide_truncate_model_get_child_model (IdeTruncateModel *self);
guint             ide_truncate_model_get_max_items   (IdeTruncateModel *self);
void              ide_truncate_model_set_max_items   (IdeTruncateModel *self,
                                                      guint             max_items);
gboolean          ide_truncate_model_get_can_expand  (IdeTruncateModel *self);
gboolean          ide_truncate_model_get_expanded    (IdeTruncateModel *self);
void              ide_truncate_model_set_expanded    (IdeTruncateModel *self,
                                                      gboolean          expanded);

G_END_DECLS
