/* egg-binding-set.c
 *
 * Copyright (C) 2015 Christian Hergert <christian@hergert.me>
 *
 * This file is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 3 of the
 * License, or (at your option) any later version.
 *
 * This file is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define G_LOG_DOMAIN "egg-binding-set"

#include <glib/gi18n.h>

#include "egg-binding-set.h"

/**
 * SECTION:egg-binding-set
 * @title: EggBindingSet
 * @short_description: Manage multiple #GBinding as a set.
 *
 * This should not be confused with #GtkBindingSet.
 *
 * #EggBindingSet allows you to manage a set of #GBindings that you
 * would like attached to the same source object. This is convenience
 * so that you can manage them as a set rather than reconnecting them
 * individually.
 */

struct _EggBindingSet
{
  GObject    parent_instance;

  GObject   *source;
  GPtrArray *lazy_bindings;
};

typedef struct
{
  EggBindingSet         *set;
  const gchar           *source_property;
  const gchar           *target_property;
  GObject               *target;
  GBinding              *binding;
  gpointer               user_data;
  GDestroyNotify         user_data_destroy;
  GBindingTransformFunc  transform_to;
  GBindingTransformFunc  transform_from;
  GClosure              *transform_to_closure;
  GClosure              *transform_from_closure;
  GBindingFlags          binding_flags;
} LazyBinding;

G_DEFINE_TYPE (EggBindingSet, egg_binding_set, G_TYPE_OBJECT)

enum {
  PROP_0,
  PROP_SOURCE,
  LAST_PROP
};

static GParamSpec *gParamSpecs [LAST_PROP];

/*#define DEBUG_BINDINGS 1*/

#ifdef DEBUG_BINDINGS
static gchar *
_g_flags_to_string (GFlagsClass *flags_class,
                    guint        value)
{
  GString *str;
  GFlagsValue *flags_value;
  gboolean first = TRUE;

  str = g_string_new (NULL);

  while ((first || value != 0) &&
         (flags_value = g_flags_get_first_value (flags_class, value)) != NULL)
    {
      if (!first)
        g_string_append (str, " | ");

      g_string_append (str, flags_value->value_name);

      first = FALSE;
      value &= ~(flags_value->value);
    }

  return g_string_free (str, FALSE);
}
#endif

static void
egg_binding_set_connect (EggBindingSet *self,
                         LazyBinding   *lazy_binding)
{
  GBinding *binding;

  g_assert (EGG_IS_BINDING_SET (self));
  g_assert (self->source != NULL);
  g_assert (lazy_binding != NULL);
  g_assert (lazy_binding->binding == NULL);
  g_assert (lazy_binding->target != NULL);
  g_assert (lazy_binding->target_property != NULL);
  g_assert (lazy_binding->source_property != NULL);

#ifdef DEBUG_BINDINGS
  {
    GFlagsClass *flags_class;
    g_autofree gchar *flags_str;

    flags_class = g_type_class_ref (G_TYPE_BINDING_FLAGS);
    flags_str = _g_flags_to_string (flags_class,
                                    lazy_binding->binding_flags);

    g_print ("Binding %s(%p):%s to %s(%p):%s (flags=%s)\n",
             G_OBJECT_TYPE_NAME (self->source),
             self->source,
             lazy_binding->source_property,
             G_OBJECT_TYPE_NAME (lazy_binding->target),
             lazy_binding->target,
             lazy_binding->target_property,
             flags_str);

    g_type_class_unref (flags_class);
  }
#endif

  if (lazy_binding->transform_to_closure == NULL &&
      lazy_binding->transform_from_closure == NULL)
    {
      binding = g_object_bind_property_full (self->source,
                                             lazy_binding->source_property,
                                             lazy_binding->target,
                                             lazy_binding->target_property,
                                             lazy_binding->binding_flags,
                                             lazy_binding->transform_to,
                                             lazy_binding->transform_from,
                                             lazy_binding->user_data,
                                             NULL);
    }
  else
    {
      binding = g_object_bind_property_with_closures (self->source,
                                                      lazy_binding->source_property,
                                                      lazy_binding->target,
                                                      lazy_binding->target_property,
                                                      lazy_binding->binding_flags,
                                                      lazy_binding->transform_to_closure,
                                                      lazy_binding->transform_from_closure);
    }

  lazy_binding->binding = binding;
}

static void
egg_binding_set_disconnect (LazyBinding *lazy_binding)
{
  g_assert (lazy_binding != NULL);

  if (lazy_binding->binding != NULL)
    {
      g_binding_unbind (lazy_binding->binding);
      lazy_binding->binding = NULL;
    }
}

static void
egg_binding_set__source_weak_notify (gpointer  data,
                                     GObject  *where_object_was)
{
  EggBindingSet *self = data;
  gsize i;

  g_assert (EGG_IS_BINDING_SET (self));

  self->source = NULL;

  for (i = 0; i < self->lazy_bindings->len; i++)
    {
      LazyBinding *lazy_binding;

      lazy_binding = g_ptr_array_index (self->lazy_bindings, i);
      lazy_binding->binding = NULL;
    }
}

static void
egg_binding_set__target_weak_notify (gpointer  data,
                                     GObject  *where_object_was)
{
  EggBindingSet *self = data;
  gsize i;

  g_assert (EGG_IS_BINDING_SET (self));

  for (i = 0; i < self->lazy_bindings->len; i++)
    {
      LazyBinding *lazy_binding;

      lazy_binding = g_ptr_array_index (self->lazy_bindings, i);

      if (lazy_binding->target == where_object_was)
        {
          lazy_binding->target = NULL;
          lazy_binding->binding = NULL;

          g_ptr_array_remove_index_fast (self->lazy_bindings, i);
          break;
        }
    }
}

static void
lazy_binding_free (gpointer data)
{
  LazyBinding *lazy_binding = data;

  if (lazy_binding->target != NULL)
    {
      g_object_weak_unref (lazy_binding->target,
                           egg_binding_set__target_weak_notify,
                           lazy_binding->set);
      lazy_binding->target = NULL;
    }

  egg_binding_set_disconnect (lazy_binding);

  lazy_binding->set = NULL;
  lazy_binding->source_property = NULL;
  lazy_binding->target_property = NULL;

  if (lazy_binding->user_data_destroy)
    lazy_binding->user_data_destroy (lazy_binding->user_data);

  g_clear_pointer (&lazy_binding->transform_to_closure, g_closure_unref);
  g_clear_pointer (&lazy_binding->transform_from_closure, g_closure_unref);

  g_slice_free (LazyBinding, lazy_binding);
}

static void
egg_binding_set_dispose (GObject *object)
{
  EggBindingSet *self = (EggBindingSet *)object;

  g_assert (EGG_IS_BINDING_SET (self));

  if (self->source != NULL)
    {
      g_object_weak_unref (self->source,
                           egg_binding_set__source_weak_notify,
                           self);
      self->source = NULL;
    }

  if (self->lazy_bindings->len != 0)
    g_ptr_array_remove_range (self->lazy_bindings, 0, self->lazy_bindings->len);

  G_OBJECT_CLASS (egg_binding_set_parent_class)->dispose (object);
}

static void
egg_binding_set_finalize (GObject *object)
{
  EggBindingSet *self = (EggBindingSet *)object;

  g_assert (self->lazy_bindings != NULL);
  g_assert (self->lazy_bindings->len == 0);

  g_clear_pointer (&self->lazy_bindings, g_ptr_array_unref);

  G_OBJECT_CLASS (egg_binding_set_parent_class)->finalize (object);
}

static void
egg_binding_set_get_property (GObject    *object,
                              guint       prop_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
  EggBindingSet *self = EGG_BINDING_SET (object);

  switch (prop_id)
    {
    case PROP_SOURCE:
      g_value_set_object (value, egg_binding_set_get_source (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
egg_binding_set_set_property (GObject      *object,
                              guint         prop_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
  EggBindingSet *self = EGG_BINDING_SET (object);

  switch (prop_id)
    {
    case PROP_SOURCE:
      egg_binding_set_set_source (self, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
egg_binding_set_class_init (EggBindingSetClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = egg_binding_set_dispose;
  object_class->finalize = egg_binding_set_finalize;
  object_class->get_property = egg_binding_set_get_property;
  object_class->set_property = egg_binding_set_set_property;

  gParamSpecs [PROP_SOURCE] =
    g_param_spec_object ("source",
                         _("Source"),
                         _("The source GObject."),
                         G_TYPE_OBJECT,
                         (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_properties (object_class, LAST_PROP, gParamSpecs);
}

static void
egg_binding_set_init (EggBindingSet *self)
{
  self->lazy_bindings = g_ptr_array_new_with_free_func (lazy_binding_free);
}

/**
 * egg_binding_set_new:
 *
 * Creates a new #EggBindingSet.
 *
 * Returns: a new #EggBindingSet
 */
EggBindingSet *
egg_binding_set_new (void)
{
  return g_object_new (EGG_TYPE_BINDING_SET, NULL);
}

/**
 * egg_binding_set_get_source:
 * @self: the #EggBindingSet
 *
 * Gets the source object used for binding properties.
 *
 * Returns: (nullable): the source object.
 */
GObject *
egg_binding_set_get_source (EggBindingSet *self)
{
  g_return_val_if_fail (EGG_IS_BINDING_SET (self), NULL);

  return self->source;
}

static gboolean
egg_binding_set_check_source (EggBindingSet *self,
                              gpointer       source)
{
  gsize i;

  for (i = 0; i < self->lazy_bindings->len; i++)
    {
      LazyBinding *lazy_binding;

      lazy_binding = g_ptr_array_index (self->lazy_bindings, i);

      g_return_val_if_fail (g_object_class_find_property (G_OBJECT_GET_CLASS (source),
                                                          lazy_binding->source_property) != NULL,
                            FALSE);
    }

  return TRUE;
}

/**
 * egg_binding_set_set_source:
 * @self: the #EggBindingSet
 * @source: (type GObject) (nullable): the source #GObject
 *
 * Sets @source as the source object used for creating property
 * bindings. If there is already a source object all bindings from it
 * will be removed.
 *
 * Note: All properties that have been bound must exist on @source.
 */
void
egg_binding_set_set_source (EggBindingSet *self,
                            gpointer       source)
{
  g_return_if_fail (EGG_IS_BINDING_SET (self));
  g_return_if_fail (!source || G_IS_OBJECT (source));
  g_return_if_fail (source != (gpointer)self);

  if (source == (gpointer)self->source)
    return;

  if (self->source != NULL)
    {
      gsize i;

      g_object_weak_unref (self->source,
                           egg_binding_set__source_weak_notify,
                           self);
      self->source = NULL;

      for (i = 0; i < self->lazy_bindings->len; i++)
        {
          LazyBinding *lazy_binding;

          lazy_binding = g_ptr_array_index (self->lazy_bindings, i);
          egg_binding_set_disconnect (lazy_binding);
        }
    }

  if (source != NULL && egg_binding_set_check_source (self, source))
    {
      gsize i;

      self->source = source;
      g_object_weak_ref (self->source,
                         egg_binding_set__source_weak_notify,
                         self);

      for (i = 0; i < self->lazy_bindings->len; i++)
        {
          LazyBinding *lazy_binding;

          lazy_binding = g_ptr_array_index (self->lazy_bindings, i);
          egg_binding_set_connect (self, lazy_binding);
        }
    }

  g_object_notify_by_pspec (G_OBJECT (self), gParamSpecs [PROP_SOURCE]);
}

/**
 * egg_binding_set_bind:
 * @self: the #EggBindingSet
 * @source_property: the property on the source to bind
 * @target: (type GObject): the target #GObject
 * @target_property: the property on @target to bind
 * @flags: the flags used to create the #GBinding
 *
 * Creates a binding between @source_property on the source object
 * and @target_property on @target. Whenever the @source_property
 * is changed the @target_property is updated using the same value.
 * The binding flags #G_BINDING_SYNC_CREATE is automatically specified.
 *
 * See: g_object_bind_property().
 */
void
egg_binding_set_bind (EggBindingSet *self,
                      const gchar   *source_property,
                      gpointer       target,
                      const gchar   *target_property,
                      GBindingFlags  flags)
{
  egg_binding_set_bind_full (self, source_property,
                             target, target_property,
                             flags,
                             NULL, NULL,
                             NULL, NULL);
}

/**
 * egg_binding_set_bind_full:
 * @self: the #EggBindingSet
 * @source_property: the property on the source to bind
 * @target: (type GObject): the target #GObject
 * @target_property: the property on @target to bind
 * @flags: the flags used to create the #GBinding
 * @transform_to: (scope notified) (nullable): the transformation function
 *     from the source object to the @target, or %NULL to use the default
 * @transform_from: (scope notified) (nullable): the transformation function
 *     from the @target to the source object, or %NULL to use the default
 * @user_data: custom data to be passed to the transformation
 *             functions, or %NULL
 * @user_data_destroy: function to be called when disposing the binding,
 *     to free the resources used by the transformation functions
 *
 * Creates a binding between @source_property on the source object and
 * @target_property on @target, allowing you to set the transformation
 * functions to be used by the binding. The binding flags
 * #G_BINDING_SYNC_CREATE is automatically specified.
 *
 * See: g_object_bind_property_full().
 */
void
egg_binding_set_bind_full (EggBindingSet         *self,
                           const gchar           *source_property,
                           gpointer               target,
                           const gchar           *target_property,
                           GBindingFlags          flags,
                           GBindingTransformFunc  transform_to,
                           GBindingTransformFunc  transform_from,
                           gpointer               user_data,
                           GDestroyNotify         user_data_destroy)
{
  LazyBinding *lazy_binding;

  g_return_if_fail (EGG_IS_BINDING_SET (self));
  g_return_if_fail (source_property != NULL);
  g_return_if_fail (self->source == NULL ||
                    g_object_class_find_property (G_OBJECT_GET_CLASS (self->source),
                                                  source_property) != NULL);
  g_return_if_fail (G_IS_OBJECT (target));
  g_return_if_fail (target_property != NULL);
  g_return_if_fail (g_object_class_find_property (G_OBJECT_GET_CLASS (target),
                                                  target_property) != NULL);
  g_return_if_fail (target != (gpointer)self ||
                    strcmp (source_property, target_property) != 0);

  lazy_binding = g_slice_new0 (LazyBinding);
  lazy_binding->set = self;
  lazy_binding->source_property = g_intern_string (source_property);
  lazy_binding->target_property = g_intern_string (target_property);
  lazy_binding->target = target;
  lazy_binding->binding_flags = flags | G_BINDING_SYNC_CREATE;
  lazy_binding->transform_to = transform_to;
  lazy_binding->transform_from = transform_from;
  lazy_binding->user_data = user_data;
  lazy_binding->user_data_destroy = user_data_destroy;

  g_object_weak_ref (target,
                     egg_binding_set__target_weak_notify,
                     self);

  g_ptr_array_add (self->lazy_bindings, lazy_binding);

  if (self->source != NULL)
    egg_binding_set_connect (self, lazy_binding);
}

/**
 * egg_binding_set_bind_with_closures: (rename-to egg_binding_set_bind_full)
 * @self: the #EggBindingSet
 * @source_property: the property on the source to bind
 * @target: (type GObject): the target #GObject
 * @target_property: the property on @target to bind
 * @flags: the flags used to create the #GBinding
 * @transform_to: (nullable): a #GClosure wrapping the
 *     transformation function from the source object to the @target,
 *     or %NULL to use the default
 * @transform_from: (nullable): a #GClosure wrapping the
 *     transformation function from the @target to the source object,
 *     or %NULL to use the default
 *
 * Creates a binding between @source_property on the source object and
 * @target_property on @target, allowing you to set the transformation
 * functions to be used by the binding. The binding flags
 * #G_BINDING_SYNC_CREATE is automatically specified.
 *
 * This function is the language bindings friendly version of
 * egg_binding_set_bind_property_full(), using #GClosures
 * instead of function pointers.
 *
 * See: g_object_bind_property_with_closures().
 */
void
egg_binding_set_bind_with_closures (EggBindingSet *self,
                                    const gchar   *source_property,
                                    gpointer       target,
                                    const gchar   *target_property,
                                    GBindingFlags  flags,
                                    GClosure      *transform_to,
                                    GClosure      *transform_from)
{
  LazyBinding *lazy_binding;

  g_return_if_fail (EGG_IS_BINDING_SET (self));
  g_return_if_fail (source_property != NULL);
  g_return_if_fail (self->source == NULL ||
                    g_object_class_find_property (G_OBJECT_GET_CLASS (self->source),
                                                  source_property) != NULL);
  g_return_if_fail (G_IS_OBJECT (target));
  g_return_if_fail (target_property != NULL);
  g_return_if_fail (g_object_class_find_property (G_OBJECT_GET_CLASS (target),
                                                  target_property) != NULL);
  g_return_if_fail (target != (gpointer)self ||
                    strcmp (source_property, target_property) != 0);

  lazy_binding = g_slice_new0 (LazyBinding);
  lazy_binding->set = self;
  lazy_binding->source_property = g_intern_string (source_property);
  lazy_binding->target_property = g_intern_string (target_property);
  lazy_binding->target = target;
  lazy_binding->binding_flags = flags | G_BINDING_SYNC_CREATE;
  lazy_binding->transform_to_closure = transform_to;
  lazy_binding->transform_from_closure = transform_from;

  if (transform_to != NULL)
    g_closure_sink (g_closure_ref (transform_to));

  if (transform_from != NULL)
    g_closure_sink (g_closure_ref (transform_from));

  g_object_weak_ref (target,
                     egg_binding_set__target_weak_notify,
                     self);

  g_ptr_array_add (self->lazy_bindings, lazy_binding);

  if (self->source != NULL)
    egg_binding_set_connect (self, lazy_binding);
}
