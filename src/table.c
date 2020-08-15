#include "private_api.h"

static
ecs_data_t* init_data(
    ecs_world_t *world,
    ecs_table_t *table,
    ecs_data_t *result)
{
    ecs_type_t type = table->type; 
    int32_t i, count = table->column_count, sw_count = table->sw_column_count;
    
    result->entities = NULL;
    result->record_ptrs = NULL;
    result->marked_dirty = false;

    /* Root tables don't have columns */
    if (!count && !sw_count) {
        result->columns = NULL;
        return result;
    }

    ecs_entity_t *entities = ecs_vector_first(type, ecs_entity_t);

    if (count && !sw_count) {
        result->columns = ecs_os_calloc(ECS_SIZEOF(ecs_column_t) * count);    
    } else if (count || sw_count) {
        /* If a table has switch columns, store vector with the case values
            * as a regular column, so it's easier to access for systems. To
            * enable this, we need to allocate more space. */
        int32_t type_count = ecs_vector_count(type);
        result->columns = ecs_os_calloc(ECS_SIZEOF(ecs_column_t) * type_count);
    }

    if (count) {
        for (i = 0; i < count; i ++) {
            ecs_entity_t e = entities[i];

            /* Is the column a component? */
            const EcsComponent *component = ecs_component_from_id(world, e);
            if (component) {
                /* Is the component associated wit a (non-empty) type? */
                if (component->size) {
                    /* This is a regular component column */
                    result->columns[i].size = ecs_to_i16(component->size);
                    result->columns[i].alignment = ecs_to_i16(component->alignment);
                } else {
                    /* This is a tag */
                }
            } else {
                /* This is an entity that was added to the type */
            }
        }
    }

    if (sw_count) {
        int32_t sw_offset = table->sw_column_offset;
        result->sw_columns = ecs_os_calloc(ECS_SIZEOF(ecs_column_t) * sw_count);

        for (i = 0; i < sw_count; i ++) {
            ecs_entity_t e = entities[i + sw_offset];
            ecs_assert(ECS_HAS_ROLE(e, SWITCH), ECS_INTERNAL_ERROR, NULL);
            e = e & ECS_ENTITY_MASK;
            const EcsType *type_ptr = ecs_get(world, e, EcsType);
            ecs_assert(type_ptr != NULL, ECS_INTERNAL_ERROR, NULL);
            ecs_type_t sw_type = type_ptr->normalized;

            ecs_entity_t *sw_array = ecs_vector_first(sw_type, ecs_entity_t);
            int32_t sw_array_count = ecs_vector_count(sw_type);

            ecs_switch_t *sw = ecs_switch_new(
                sw_array[0], 
                sw_array[sw_array_count - 1], 
                0);
            result->sw_columns[i].data = sw;
            result->sw_columns[i].type = sw_type;

            int32_t column_id = i + table->sw_column_offset;
            result->columns[column_id].data = ecs_switch_values(sw);
            result->columns[column_id].size = sizeof(ecs_entity_t);
            result->columns[column_id].alignment = ECS_ALIGNOF(ecs_entity_t);
        }
    }

    return result;
}

/* Delete data for a stage */
static
void deinit_data(
    ecs_table_t *table,
    ecs_data_t *data)
{
    ecs_column_t *columns = data->columns;
    if (columns) {
        int32_t c, column_count = table->column_count;
        for (c = 0; c < column_count; c ++) {
            ecs_vector_free(columns[c].data);
        }
        ecs_os_free(columns);
        data->columns = NULL;
    }

    ecs_vector_free(data->entities);
    ecs_vector_free(data->record_ptrs);

    data->entities = NULL;
    data->record_ptrs = NULL;
}

/* Utility function to free data for all stages */
static
void deinit_all_data(
    ecs_table_t *table)
{
    ecs_data_t *data = ecs_vector_first(table->data, ecs_data_t);
    int32_t i, count = ecs_vector_count(table->data);
    
    for (i = 0; i < count; i ++) {
        deinit_data(table, &data[i]);
    }

    ecs_vector_free(table->data);

    table->data = NULL; 
}

static
ecs_flags32_t get_component_action_flags(
    ecs_c_info_t *c_info) 
{
    ecs_flags32_t flags = 0;

    if (c_info->lifecycle.ctor) {
        flags |= EcsTableHasCtors;
    }
    if (c_info->lifecycle.dtor) {
        flags |= EcsTableHasDtors;
    }
    if (c_info->lifecycle.copy) {
        flags |= EcsTableHasCopy;
    }
    if (c_info->lifecycle.move) {
        flags |= EcsTableHasMove;
    }
    if (c_info->on_add) {
        flags |= EcsTableHasOnAdd;
    }
    if (c_info->on_remove) {
        flags |= EcsTableHasOnRemove;
    }    

    return flags;  
}

static
void notify_component_info(
    ecs_world_t *world,
    ecs_table_t *table,
    ecs_entity_t component)
{
    ecs_type_t table_type = table->type;
    if (!component || ecs_type_owns_entity(world, table_type, component, true)){
        int32_t column_count = ecs_vector_count(table_type);
        ecs_assert(!component || column_count != 0, ECS_INTERNAL_ERROR, NULL);

        if (!column_count) {
            return;
        }
        
        if (!table->c_info) {
            table->c_info = ecs_os_calloc(sizeof(ecs_c_info_t*) * column_count);
        }

        /* Reset lifecycle flags before recomputing */
        table->flags &= ~EcsTableHasLifecycle;

        /* Recompute lifecycle flags */
        ecs_entity_t *array = ecs_vector_first(table_type, ecs_entity_t);
        int32_t i;
        for (i = 0; i < column_count; i ++) {
            ecs_entity_t c = array[i];
            ecs_c_info_t *c_info = ecs_get_c_info(world, c);
            if (c_info) {
                ecs_flags32_t flags = get_component_action_flags(c_info);
                table->flags |= flags;
            }

            /* Store pointer to c_info for fast access */
            table->c_info[i] = c_info;
        }        
    }
}

static
void run_un_set_handlers(
    ecs_world_t *world,
    ecs_table_t *table,
    ecs_data_t *data)
{
    int32_t count = ecs_vector_count(data->entities);

    if (count) {
        ecs_run_monitors(world, &world->stage, table, table->un_set_all, 
            0, count, NULL);
    }
}

static
void run_remove_actions(
    ecs_world_t *world,
    ecs_table_t *table,
    ecs_data_t *data,
    int32_t row,
    int32_t count,
    bool dtor_only)
{
    if (count) {
        ecs_entities_t components = ecs_type_to_entities(table->type);
        ecs_column_info_t *cinfo = NULL;
        ecs_column_info_t cinfo_buff[ECS_MAX_ADD_REMOVE];
        if (components.count < ECS_MAX_ADD_REMOVE) {
            cinfo = cinfo_buff;
            ecs_get_column_info(world, table, &components, cinfo, true);
        } else {
            cinfo = ecs_os_malloc(ECS_SIZEOF(ecs_column_info_t) * components.count);
            ecs_get_column_info(world, table, &components, cinfo, true);
        }

        if (!dtor_only) {
            ecs_run_monitors(world, &world->stage, table, NULL, 
                    row, count, table->un_set_all);
        }

        /* Run deinit actions (dtors) for components. Don't run triggers */
        ecs_components_destruct(world, &world->stage, data, row, count, 
            cinfo, components.count);

        if (cinfo != cinfo_buff) {
            ecs_os_free(cinfo);
        }
    }
}

void ecs_table_destruct(
    ecs_world_t *world, 
    ecs_table_t *table, 
    ecs_data_t *data, 
    int32_t row, 
    int32_t count)
{
    if (table->flags & EcsTableHasDtors) {
        run_remove_actions(world, table, data, row, count, true);
    }
}

static
int compare_matched_query(
    const void *ptr1,
    const void *ptr2)
{
    const ecs_matched_query_t *m1 = ptr1;
    const ecs_matched_query_t *m2 = ptr2;
    ecs_query_t *q1 = m1->query;
    ecs_query_t *q2 = m2->query;
    ecs_assert(q1 != NULL, ECS_INTERNAL_ERROR, NULL);
    ecs_assert(q2 != NULL, ECS_INTERNAL_ERROR, NULL);

    ecs_entity_t s1 = q1->system;
    ecs_entity_t s2 = q2->system;
    ecs_assert(s1 != 0, ECS_INTERNAL_ERROR, NULL);
    ecs_assert(s2 != 0, ECS_INTERNAL_ERROR, NULL);

    return (s1 > s2) - (s1 < s2);
}

static
void add_monitor(
    ecs_vector_t **array,
    ecs_query_t *query,
    int32_t matched_table_index)
{
    /* Add the system to a list that contains all OnSet systems matched with
     * this table. This makes it easy to get the list of systems that need to be
     * executed when all components are set, like when new_w_data is used */
    ecs_matched_query_t *m = ecs_vector_add(array, ecs_matched_query_t);
    ecs_assert(m != NULL, ECS_INTERNAL_ERROR, NULL);

    m->query = query;
    m->matched_table_index = matched_table_index;

    /* Sort the system list so that it is easy to get the difference OnSet
     * OnSet systems between two tables. */
    qsort(
        ecs_vector_first(*array, ecs_matched_query_t), 
        ecs_to_size_t(ecs_vector_count(*array)),
        ECS_SIZEOF(ecs_matched_query_t), 
        compare_matched_query);
}

/* This function is called when a query is matched with a table. A table keeps
 * a list of tables that match so that they can be notified when the table
 * becomes empty / non-empty. */
static
void register_monitor(
    ecs_world_t *world,
    ecs_table_t *table,
    ecs_query_t *query,
    int32_t matched_table_index)
{
    (void)world;
    ecs_assert(query != NULL, ECS_INTERNAL_ERROR, NULL);

    /* First check if system is already registered as monitor. It is possible
     * the query just wants to update the matched_table_index (for example, if
     * query tables got reordered) */
    ecs_vector_each(table->monitors, ecs_matched_query_t, m, {
        if (m->query == query) {
            m->matched_table_index = matched_table_index;
            return;
        }
    });

    add_monitor(&table->monitors, query, matched_table_index);

#ifndef NDEBUG
    char *str = ecs_type_str(world, table->type);
    ecs_trace_2("monitor #[green]%s#[reset] registered with table #[red]%s",
        ecs_get_name(world, query->system), str);
    ecs_os_free(str);
#endif
}

static
bool is_override(
    ecs_world_t *world,
    ecs_table_t *table,
    ecs_entity_t comp)
{
    if (!(table->flags & EcsTableHasBase)) {
        return false;
    }

    ecs_type_t type = table->type;
    int32_t i, count = ecs_vector_count(type);
    ecs_entity_t *entities = ecs_vector_first(type, ecs_entity_t);

    for (i = count - 1; i >= 0; i --) {
        ecs_entity_t e = entities[i];
        if (ECS_HAS_ROLE(e, INSTANCEOF)) {
            if (ecs_has_entity(world, e & ECS_ENTITY_MASK, comp)) {
                return true;
            }
        } else {
            /* ECS_INSTANCEOF will always appear at the end of a type */
            return false;
        }
    }

    return false;
}

static
void register_on_set(
    ecs_world_t *world,
    ecs_table_t *table,
    ecs_query_t *query,
    int32_t matched_table_index)
{
    (void)world;

    if (table->column_count) {
        if (!table->on_set) {
            table->on_set = ecs_os_calloc(ECS_SIZEOF(ecs_vector_t) * table->column_count);
        }

        /* Keep track of whether query matches overrides. When a component is
         * removed, diffing these arrays between the source and detination
         * tables gives the list of OnSet systems to run, after exposing the
         * component that was overridden. */
        bool match_override = false;

        /* Add system to each matched column. This makes it easy to get the list of
        * systems when setting a single component. */
        ecs_vector_each(query->sig.columns, ecs_sig_column_t, column, {
            ecs_sig_oper_kind_t oper_kind = column->oper_kind;
            ecs_sig_from_kind_t from_kind = column->from_kind;

            if (from_kind != EcsFromAny && from_kind != EcsFromOwned) {
                continue;
            }

            if (oper_kind == EcsOperAnd || oper_kind == EcsOperOptional) {
                ecs_entity_t comp = column->is.component;
                int32_t index = ecs_type_index_of(table->type, comp);
                if (index == -1) {
                    continue;
                }

                if (index >= table->column_count) {
                    continue;
                }
                
                ecs_vector_t *set_c = table->on_set[index];
                ecs_matched_query_t *m = ecs_vector_add(&set_c, ecs_matched_query_t);
                m->query = query;
                m->matched_table_index = matched_table_index;
                table->on_set[index] = set_c;
                
                match_override |= is_override(world, table, comp);
            }
        });   

        if (match_override) {
            add_monitor(&table->on_set_override, query, matched_table_index);
        }
    }

    add_monitor(&table->on_set_all, query, matched_table_index);   
}

static
void register_un_set(
    ecs_world_t *world,
    ecs_table_t *table,
    ecs_query_t *query,
    int32_t matched_table_index)
{
    (void)world;
    add_monitor(&table->un_set_all, query, matched_table_index);
}

/* -- Private functions -- */

/* If table goes from 0 to >0 entities or from >0 entities to 0 entities notify
 * queries. This allows systems associated with queries to move inactive tables
 * out of the main loop. */
void ecs_table_activate(
    ecs_world_t *world,
    ecs_table_t *table,
    ecs_query_t *query,
    bool activate)
{
    if (query) {
        ecs_query_notify(world, query, &(ecs_query_event_t) {
            .kind = activate ? EcsQueryTableNonEmpty : EcsQueryTableEmpty,
            .table = table
        });
    } else {
        ecs_vector_t *queries = table->queries;
        ecs_query_t **buffer = ecs_vector_first(queries, ecs_query_t*);
        int32_t i, count = ecs_vector_count(queries);
        for (i = 0; i < count; i ++) {
            ecs_query_notify(world, buffer[i], &(ecs_query_event_t) {
                .kind = activate ? EcsQueryTableNonEmpty : EcsQueryTableEmpty,
                .table = table
            });                
        }
    }     
}

/* This function is called when a query is matched with a table. A table keeps
 * a list of tables that match so that they can be notified when the table
 * becomes empty / non-empty. */
static
void register_query(
    ecs_world_t *world,
    ecs_table_t *table,
    ecs_query_t *query,
    int32_t matched_table_index)
{
    /* Register system with the table */
    if (!(query->flags & EcsQueryNoActivation)) {
        ecs_query_t **q = ecs_vector_add(&table->queries, ecs_query_t*);
        if (q) *q = query;

        ecs_data_t *data = ecs_table_get_data(world, table);
        if (data && ecs_vector_count(data->entities)) {
            ecs_table_activate(world, table, query, true);
        }
    }

    /* Register the query as a monitor */
    if (query->flags & EcsQueryMonitor) {
        table->flags |= EcsTableHasMonitors;
        register_monitor(world, table, query, matched_table_index);
    }

    /* Register the query as an on_set system */
    if (query->flags & EcsQueryOnSet) {
        register_on_set(world, table, query, matched_table_index);
    }

    /* Register the query as an un_set system */
    if (query->flags & EcsQueryUnSet) {
        register_un_set(world, table, query, matched_table_index);
    }
}

static
ecs_data_t* get_data_intern(
    ecs_world_t *world,
    ecs_stage_t *stage,
    ecs_table_t *table,
    bool create)
{
    ecs_assert(table != NULL, ECS_INTERNAL_ERROR, NULL);

    ecs_vector_t *data = table->data;

    /* If the table doesn't contain any staged data and we're not asked to
     * create data, don't allocate the array. This will reduce memory footprint
     * for tables that don't contain data but are used for graph traversal. */
    if (!data && !create) {
        return NULL;
    }

    ecs_data_t *data_array = ecs_vector_first(data, ecs_data_t);
    int32_t count = ecs_vector_count(data);
    int32_t stage_count = world->stage_count;
    int32_t id = stage->id;

    ecs_assert(id < stage_count, ECS_INTERNAL_ERROR, NULL);
    ecs_assert(!create || id || !world->in_progress, ECS_INTERNAL_ERROR, NULL);

    /* Make sure the array is large enough for the number of active stages. This
     * guarantees that any pointers returned by this function are stable, unless
     * the number of stages changes (can happen when the number of worker 
     * threads changes) */
    if (count != stage_count) {
        if (stage_count > count) {
            /* Grow array, initialize table data to 0 */
            ecs_vector_set_count(&table->data, ecs_data_t, stage_count);
            data_array = ecs_vector_first(table->data, ecs_data_t);
            ecs_os_memset(&data_array[count], 
                0, ECS_SIZEOF(ecs_data_t) * (stage_count - count));
        } else {
            /* If the number of stages is reduced, deinit redudant stages */
            int i;
            for (i = stage_count; i < count; i ++) {
                deinit_data(table, &data_array[i]);
            }

            ecs_vector_set_count(&table->data, ecs_data_t, stage_count);
            data_array = ecs_vector_first(table->data, ecs_data_t);           
        }
    }
    
    return &data_array[id];
}

ecs_data_t* ecs_table_get_data(
    ecs_world_t *world,
    ecs_table_t *table)
{
    return get_data_intern(world, &world->stage, table, false);
}

ecs_data_t* ecs_table_get_staged_data(
    ecs_world_t *world,
    ecs_stage_t *stage,
    ecs_table_t *table)
{
    return get_data_intern(world, stage, table, false);
}

ecs_data_t* ecs_table_get_or_create_data(
    ecs_world_t *world,
    ecs_stage_t *stage,
    ecs_table_t *table)
{
    ecs_data_t *result = get_data_intern(world, stage, table, true);

    /* If write access is requested from table and stage is not the main 
     * stage mark this table as dirty as it will contain staged data. */
    if (world->in_progress && !result->marked_dirty) {
        ecs_table_t** table_ptr = ecs_vector_add(
                &stage->dirty_tables, ecs_table_t*);
        *table_ptr = table;

        /* Don't add table multiple times. Value is reset during merge */
        result->marked_dirty = true;
    }

    return result;   
}

void ecs_table_clear_data(
    ecs_table_t *table,
    ecs_data_t *data)
{
    deinit_data(table, data);
}

/* Clear columns. Deactivate table in systems if necessary, but do not invoke
 * OnRemove handlers. This is typically used when restoring a table to a
 * previous state. */
void ecs_table_clear_silent(
    ecs_world_t *world,
    ecs_table_t *table)
{
    ecs_data_t *data = ecs_table_get_data(world, table);
    if (!data) {
        return;
    }

    int32_t count = ecs_vector_count(data->entities);
    
    deinit_all_data(table);

    if (count) {
        ecs_table_activate(world, table, 0, false);
    }
}

/* Delete all entities in table, invoke OnRemove handlers. This function is used
 * when an application invokes delete_w_filter. Use ecs_table_clear_silent, as the
 * table may have to be deactivated with systems. */
void ecs_table_clear(
    ecs_world_t *world,
    ecs_table_t *table)
{
    ecs_data_t *data = ecs_table_get_data(world, table);
    if (data) {
        run_remove_actions(
            world, table, data, 0, ecs_table_data_count(data), false);

        ecs_entity_t *entities = ecs_vector_first(data->entities, ecs_entity_t);
        int32_t i, count = ecs_vector_count(data->entities);
        for(i = 0; i < count; i ++) {
            ecs_eis_delete(&world->stage, entities[i]);
        }
    }

    ecs_table_clear_silent(world, table);
}

/* Unset all components in table. This function is called before a table is 
 * deleted, and invokes all UnSet handlers, if any */
void ecs_table_unset(
    ecs_world_t *world,
    ecs_table_t *table)
{
    (void)world;
    ecs_data_t *data = ecs_table_get_data(world, table);
    if (data) {
        run_un_set_handlers(world, table, data);
    }   
}

/* Free table resources. Do not invoke handlers and do not activate/deactivate
 * table with systems. This function is used when the world is freed. */
void ecs_table_free(
    ecs_world_t *world,
    ecs_table_t *table)
{
    (void)world;
    ecs_data_t *data = ecs_table_get_data(world, table);
    if (data) {
        run_remove_actions(
            world, table, data, 0, ecs_table_data_count(data), false);
    }

    deinit_all_data(table);
    ecs_os_free(table->lo_edges);
    ecs_map_free(table->hi_edges);
    ecs_vector_free(table->queries);
    ecs_vector_free((ecs_vector_t*)table->type);
    ecs_os_free(table->dirty_state);
    ecs_vector_free(table->monitors);
    ecs_vector_free(table->on_set_all);
    ecs_vector_free(table->on_set_override);
    ecs_vector_free(table->un_set_all);
    
    if (table->on_set) {
        int32_t i;
        for (i = 0; i < table->column_count; i ++) {
            ecs_vector_free(table->on_set[i]);
        }
        ecs_os_free(table->on_set);
    }
}

/* Reset a table to its initial state. This is used to reset the root table of a
 * stage after a merge has occurred, so that it no longer refers to tables that
 * were created in the stage. */
void ecs_table_reset(
    ecs_world_t *world,
    ecs_table_t *table)
{
    (void)world;

    if (table->lo_edges) {
        memset(table->lo_edges, 0, ECS_SIZEOF(ecs_edge_t) * ECS_HI_COMPONENT_ID);
        ecs_map_clear(table->hi_edges);
    }
}

static
void mark_table_dirty(
    ecs_table_t *table,
    int32_t index)
{
    if (table->dirty_state) {
        table->dirty_state[index] ++;
    }
}

void ecs_table_mark_dirty_w_index(
    ecs_table_t *table,
    int32_t index)
{
    mark_table_dirty(table, index);
}

void ecs_table_mark_dirty(
    ecs_table_t *table,
    ecs_entity_t component)
{
    ecs_assert(table != NULL, ECS_INTERNAL_ERROR, NULL);
    if (table->dirty_state) {
        int32_t index = ecs_type_index_of(table->type, component);
        ecs_assert(index != -1, ECS_INTERNAL_ERROR, NULL);
        table->dirty_state[index] ++;
    }
}

static
void move_switch_columns(
    ecs_table_t *new_table, 
    ecs_data_t *new_data, 
    int32_t new_index,
    ecs_table_t *old_table, 
    ecs_data_t *old_data, 
    int32_t old_index,
    int32_t count)
{
    int32_t i_old = 0, old_column_count = old_table->sw_column_count;
    int32_t i_new = 0, new_column_count = new_table->sw_column_count;

    if (!old_column_count || !new_column_count) {
        return;
    }

    ecs_sw_column_t *old_columns = old_data->sw_columns;
    ecs_sw_column_t *new_columns = new_data->sw_columns;

    ecs_type_t new_type = new_table->type;
    ecs_type_t old_type = old_table->type;

    int32_t offset_new = new_table->sw_column_offset;
    int32_t offset_old = old_table->sw_column_offset;

    ecs_entity_t *new_components = ecs_vector_first(new_type, ecs_entity_t);
    ecs_entity_t *old_components = ecs_vector_first(old_type, ecs_entity_t);

    for (; (i_new < new_column_count) && (i_old < old_column_count);) {
        ecs_entity_t new_component = new_components[i_new + offset_new];
        ecs_entity_t old_component = old_components[i_old + offset_old];

        if (new_component == old_component) {
            ecs_switch_t *old_switch = old_columns[i_old].data;
            ecs_switch_t *new_switch = new_columns[i_new].data;

            ecs_switch_set_count(new_switch, new_index + count);

            int i;
            for (i = 0; i < count; i ++) {
                uint64_t value = ecs_switch_get(old_switch, old_index + i);
                ecs_switch_set(new_switch, new_index + i, value);
            }
        }

        i_new += new_component <= old_component;
        i_old += new_component >= old_component;
    }
}

int32_t ecs_table_append(
    ecs_world_t *world,
    ecs_table_t *table,
    ecs_data_t *data,
    ecs_entity_t entity,
    ecs_record_t *record,
    bool construct)
{
    ecs_assert(table != NULL, ECS_INTERNAL_ERROR, NULL);
    ecs_assert(data != NULL, ECS_INTERNAL_ERROR, NULL);
    int32_t column_count = table->column_count;
    int32_t sw_column_count = table->sw_column_count;
    bool realloc = false;

    if (entity > ECS_HI_ENTITY_ID) {
        record = NULL;
    }

    if (column_count || sw_column_count) {
        ecs_column_t *columns = data->columns;
        ecs_sw_column_t *sw_columns = data->sw_columns;

        /* It is possible that the table data was created without content. Now 
         * that data is going to be written to the table, initialize it */ 
        if (!columns && !sw_columns) {
            init_data(world, table, data);
            columns = data->columns;
            sw_columns = data->sw_columns;
        }

        /* Add elements to each column array */
        int32_t i;
        for (i = 0; i < column_count; i ++) {
            int16_t size = columns[i].size;
            int16_t alignment = columns[i].alignment;
            if (size) {
                ecs_vector_t *prev = columns[i].data;
                void *elem = ecs_vector_add_t(
                    &columns[i].data, size, alignment);
                realloc = realloc || (prev != columns[i].data);

                ecs_c_info_t **c_info_array, *c_info;
                ecs_xtor_t ctor;
                if (construct && (c_info_array = table->c_info) &&
                  (c_info = c_info_array[i]) && (ctor = c_info->lifecycle.ctor)) 
                {
                    ctor(world, c_info->component, &entity, elem, size, 1, 
                        c_info->lifecycle.ctx);
                }
            }
        }

        for (i = 0; i < sw_column_count; i ++) {
            ecs_switch_t *sw = sw_columns[i].data;
            ecs_switch_add(sw);
            columns[i + table->sw_column_offset].data = ecs_switch_values(sw);
        }        
    }

    /* Fist add entity to array with entity ids */
    ecs_vector_t *prev_e = data->entities;
    ecs_entity_t *e = ecs_vector_add(&data->entities, ecs_entity_t);
    ecs_assert(e != NULL, ECS_INTERNAL_ERROR, NULL);
    *e = entity;
    realloc = realloc || (prev_e != data->entities);

    /* Add record ptr to array with record ptrs */
    ecs_vector_t *prev_r = data->record_ptrs;
    ecs_record_t **r = ecs_vector_add(&data->record_ptrs, ecs_record_t*);
    ecs_assert(r != NULL, ECS_INTERNAL_ERROR, NULL);
    *r = record;
    realloc = realloc || (prev_r != data->record_ptrs);

    /* If the table is monitored indicate that there has been a change */
    mark_table_dirty(table, 0);

    int32_t index = ecs_vector_count(data->entities) - 1;

    if (!world->in_progress && !index) {
        ecs_table_activate(world, table, 0, true);
    }

    /* Keep track of alloc count */
    table->alloc_count += realloc;

    /* Return index of last added entity */
    return index;
}

void ecs_table_delete(
    ecs_world_t *world,
    ecs_stage_t *stage,
    ecs_table_t *table,
    ecs_data_t *data,
    int32_t index,
    bool destruct)
{
    ecs_assert(world != NULL, ECS_INTERNAL_ERROR, NULL);
    ecs_assert(table != NULL, ECS_INTERNAL_ERROR, NULL);
    ecs_assert(data != NULL, ECS_INTERNAL_ERROR, NULL);
    ecs_assert(stage != NULL, ECS_INTERNAL_ERROR, NULL);

    ecs_vector_t *entity_column = data->entities;
    int32_t count = ecs_vector_count(entity_column);

    ecs_assert(count > 0, ECS_INTERNAL_ERROR, NULL);
    count --;
    
    ecs_assert(index <= count, ECS_INTERNAL_ERROR, NULL);

    int32_t column_count = table->column_count;
    int32_t i;

    ecs_entity_t *entities = ecs_vector_first(entity_column, ecs_entity_t);

    /* Move each component value in array to index */
    ecs_column_t *components = data->columns;
    for (i = 0; i < column_count; i ++) {
        ecs_column_t *column = &components[i];
        int16_t size = column->size;
        int16_t alignment = column->alignment;
        if (size) {
            ecs_c_info_t **c_info_array, *c_info;
            ecs_xtor_t dtor;
            if (destruct && (c_info_array = table->c_info) &&
                (c_info = c_info_array[i]) && (dtor = c_info->lifecycle.dtor)) 
            {
                void *ptr = ecs_vector_get_t(
                    column->data, size, alignment, index);
                dtor(world, c_info->component, &entities[index], ptr, size, 1,
                    c_info->lifecycle.ctx);
            }  

            ecs_vector_remove_index_t(column->data, size, alignment, index);
        }
    }

    /* Move last entity id to index */
    ecs_entity_t entity_to_move = entities[count];
    entities[index] = entity_to_move;
    ecs_vector_remove_last(entity_column);

    /* Move last record ptr to index */
    ecs_vector_t *record_column = data->record_ptrs;     
    ecs_record_t **records = ecs_vector_first(record_column, ecs_record_t*);
    ecs_assert(count < ecs_vector_count(record_column), ECS_INTERNAL_ERROR, NULL);
    ecs_record_t *record_to_move = records[count];

    records[index] = record_to_move;
    ecs_vector_remove_last(record_column);    

    /* Update record of moved entity in entity index */
    if (index != count) {
        if (!world->in_progress && record_to_move) {
            record_to_move->row = index + 1;
            ecs_assert(record_to_move->table != NULL, ECS_INTERNAL_ERROR, NULL);
            ecs_assert(record_to_move->table == table, ECS_INTERNAL_ERROR, NULL);
            ecs_assert(stage == &world->stage, ECS_INTERNAL_ERROR, NULL);
        } else {
            ecs_record_t row;
            row.table = table;
            row.row = index + 1;
            ecs_eis_set(stage, entity_to_move, &row);
        }
    }

    /* Remove elements from switch columns */
    ecs_sw_column_t *sw_columns = data->sw_columns;
    int32_t sw_column_count = table->sw_column_count;
    for (i = 0; i < sw_column_count; i ++) {
        ecs_switch_remove(sw_columns[i].data, index);
    }    

    /* If the table is monitored indicate that there has been a change */
    mark_table_dirty(table, 0);    

    if (!world->in_progress && !count) {
        ecs_table_activate(world, table, NULL, false);
    }
}

static
void ctor_component(
    ecs_world_t *world,
    ecs_entity_t component,
    ecs_column_t *column,
    ecs_entity_t *entities,
    int32_t row,
    int32_t count)
{
    /* A new component is constructed */
    ecs_xtor_t ctor;
    ecs_c_info_t *cdata = ecs_get_c_info(world, component);
    if (cdata && (ctor = cdata->lifecycle.ctor)) {
        void *ctx = cdata->lifecycle.ctx;
        int16_t size = column->size;
        int16_t alignment = column->alignment;

        void *ptr = ecs_vector_get_t(column->data, size, alignment, row);

        ctor(world, component, entities, ptr, 
            ecs_to_size_t(size), count, ctx);
    }
}

static
void dtor_component(
    ecs_world_t *world,
    ecs_entity_t component,
    ecs_column_t *column,
    ecs_entity_t *entities,
    int32_t row,
    int32_t count)
{
    /* An old component is destructed */
    ecs_xtor_t dtor;
    ecs_c_info_t *cdata = ecs_get_c_info(world, component);
    if (cdata && (dtor = cdata->lifecycle.dtor)) {
        void *ctx = cdata->lifecycle.ctx;
        int16_t size = column->size;
        int16_t alignment = column->alignment;    

        void *ptr = ecs_vector_get_t(column->data, size, alignment, row);

        dtor(world, component, entities, ptr,
            ecs_to_size_t(size), count, ctx);
    }
}

void ecs_table_move(
    ecs_world_t *world,
    ecs_stage_t *stage,
    ecs_entity_t dst_entity,
    ecs_entity_t src_entity,
    ecs_table_t *new_table,
    ecs_data_t *new_data,
    int32_t new_index,
    ecs_table_t *old_table,
    ecs_data_t *old_data,
    int32_t old_index,
    bool same_stage)
{
    ecs_assert(new_table != NULL, ECS_INTERNAL_ERROR, NULL);
    ecs_assert(old_table != NULL, ECS_INTERNAL_ERROR, NULL);

    bool to_main_stage = !same_stage && (stage == &world->stage);
    bool same_entity = dst_entity == src_entity;

    ecs_type_t new_type = new_table->type;
    ecs_type_t old_type = old_table->type;

    int32_t i_new = 0, new_column_count = new_table->column_count;
    int32_t i_old = 0, old_column_count = old_table->column_count;
    ecs_entity_t *new_components = ecs_vector_first(new_type, ecs_entity_t);
    ecs_entity_t *old_components = ecs_vector_first(old_type, ecs_entity_t);

    ecs_assert(old_index >= 0, ECS_INTERNAL_ERROR, NULL);
    ecs_assert(new_index >= 0, ECS_INTERNAL_ERROR, NULL);

    ecs_assert(old_data != NULL, ECS_INTERNAL_ERROR, NULL);
    ecs_assert(new_data != NULL, ECS_INTERNAL_ERROR, NULL);

    ecs_column_t *old_columns = old_data->columns;
    ecs_column_t *new_columns = new_data->columns;

    for (; (i_new < new_column_count) && (i_old < old_column_count);) {
        ecs_entity_t new_component = new_components[i_new];
        ecs_entity_t old_component = old_components[i_old];

        if (new_component == old_component) {
            ecs_column_t *new_column = &new_columns[i_new];
            ecs_column_t *old_column = &old_columns[i_old];
            int16_t size = new_column->size;
            int16_t alignment = new_column->alignment;

            if (size) {
                void *dst = ecs_vector_get_t(new_column->data, size, alignment, new_index);
                void *src = ecs_vector_get_t(old_column->data, size, alignment, old_index);

                ecs_assert(dst != NULL, ECS_INTERNAL_ERROR, NULL);
                ecs_assert(src != NULL, ECS_INTERNAL_ERROR, NULL);

                if (same_stage && same_entity) {
                    /* If moving in the same stage, simply copy */
                    ecs_os_memcpy(dst, src, size); 
                } else {
                    ecs_c_info_t *cdata = ecs_get_c_info(world, new_component);
                    if (to_main_stage) {
                        /* When copying from stage to main stage, move data */
                        ecs_move_t move;
                        if (cdata && (move = cdata->lifecycle.move)) {
                            void *ctx = cdata->lifecycle.ctx;
                            move(world, new_component, &dst_entity, &src_entity, 
                                dst, src, ecs_to_size_t(size), 1, ctx);
                        } else {
                            ecs_os_memcpy(dst, src, size); 
                        }
                    } else {
                        /* When copying from main stage to stage or from one
                         * entity to another, copy data */
                        ecs_copy_t copy;
                        if (cdata && (copy = cdata->lifecycle.copy)) {
                            void *ctx = cdata->lifecycle.ctx;
                            ecs_xtor_t ctor = cdata->lifecycle.ctor;

                            /* Ctor should always be set if copy is set */
                            ecs_assert(ctor != NULL, ECS_INTERNAL_ERROR, NULL);

                            /* Construct a new value in the stage, copy the
                             * value to it */
                            ctor(world, new_component, &dst_entity, dst, 
                                ecs_to_size_t(size), 1, ctx);
                            copy(world, new_component, &dst_entity, &src_entity, 
                                dst, src, ecs_to_size_t(size), 1, ctx);
                        } else {
                            ecs_os_memcpy(dst, src, size); 
                        }
                    }
                }
            }
        } else {
            if (new_component < old_component) {
                ctor_component(world, 
                    new_component, &new_columns[i_new], &dst_entity, new_index, 1);
            } else if (same_stage) {
                /* An old component is destroyed. Never destroy components when
                 * moving from main stage to stage, as we don't want to destroy
                 * a component that has not been copied.
                 * Note that a component is never copied between different 
                 * tables when copying from stage to main stage. */
                dtor_component(world, 
                    old_component, &old_columns[i_old], &src_entity, old_index, 1);
            }
        }

        i_new += new_component <= old_component;
        i_old += new_component >= old_component;
    }

    for (; (i_new < new_column_count); i_new ++) {
        ecs_entity_t component = new_components[i_new];
        ctor_component(world, 
            component, &new_columns[i_new], &dst_entity, new_index, 1);
    }

    if (same_stage) {
        for (; (i_old < old_column_count); i_old ++) {
            ecs_entity_t component = old_components[i_old];
            dtor_component(world, 
                component, &old_columns[i_old], &src_entity, old_index, 1);
        }
    }

    move_switch_columns(
        new_table, new_data, new_index, old_table, old_data, old_index, 1);
}

int32_t ecs_table_grow(
    ecs_world_t *world,
    ecs_table_t *table,
    ecs_data_t *data,
    int32_t count,
    ecs_entity_t first_entity)
{
    ecs_assert(table != NULL, ECS_INTERNAL_ERROR, NULL);
    ecs_assert(data != NULL, ECS_INTERNAL_ERROR, NULL);

    int32_t column_count = table->column_count;
    int32_t sw_column_count = table->sw_column_count;
    ecs_column_t *columns = NULL;
    ecs_sw_column_t *sw_columns = NULL;

    if (column_count || sw_column_count) {
        columns = data->columns;
        sw_columns = data->sw_columns;

        if (!columns && !sw_columns) {
            init_data(world, table, data);
            columns = data->columns;
            sw_columns = data->sw_columns;
        }
    }

    /* Fist grow record ptr array */
    ecs_record_t **r = ecs_vector_addn(&data->record_ptrs, ecs_record_t*, count);
    ecs_assert(r != NULL, ECS_INTERNAL_ERROR, NULL);    

    /* Fist add entity to column with entity ids */
    ecs_entity_t *e = ecs_vector_addn(&data->entities, ecs_entity_t, count);
    ecs_assert(e != NULL, ECS_INTERNAL_ERROR, NULL);

    int32_t i;
    for (i = 0; i < count; i ++) {
        e[i] = first_entity + (ecs_entity_t)i;
        r[i] = NULL;
    }

    /* Add elements to each column array */
    for (i = 0; i < column_count; i ++) {
        int16_t size = columns[i].size;
        if (!size) {
            continue;
        }
        int16_t alignment = columns[i].alignment;

        ecs_vector_addn_t(&columns[i].data, size, alignment, count);
    }

    /* Add elements to each switch column */
    for (i = 0; i < sw_column_count; i ++) {
        ecs_switch_t *sw = sw_columns[i].data;
        ecs_switch_addn(sw, count);
    }    

    /* If the table is monitored indicate that there has been a change */
    mark_table_dirty(table, 0);    

    int32_t row_count = ecs_vector_count(data->entities);
    if (!world->in_progress && row_count == count) {
        ecs_table_activate(world, table, 0, true);
    }

    table->alloc_count ++;

    /* Return index of first added entity */
    return row_count - count;
}

int16_t ecs_table_set_size(
    ecs_world_t *world,
    ecs_table_t *table,
    ecs_data_t *data,
    int32_t count)
{
    ecs_assert(table != NULL, ECS_INTERNAL_ERROR, NULL);
    ecs_assert(data != NULL, ECS_INTERNAL_ERROR, NULL);

    ecs_column_t *columns = data->columns;
    int32_t i, column_count = table->column_count;

    if (!columns && column_count) {
        init_data(world, table, data);
        columns = data->columns;
    }

    ecs_vector_set_size(&data->entities, ecs_entity_t, count);
    ecs_vector_set_size(&data->record_ptrs, ecs_record_t*, count);

    for (i = 0; i < column_count; i ++) {
        int16_t size = columns[i].size;
        int16_t alignment = columns[i].alignment;
        if (size) {
            ecs_vector_set_size_t(&columns[i].data, size, alignment, count);
        }
    }
    
    table->alloc_count ++;

    return 0;
}

int16_t ecs_table_set_count(
    ecs_world_t *world,
    ecs_table_t *table,
    ecs_data_t *data,
    int32_t count)
{
    ecs_assert(table != NULL, ECS_INTERNAL_ERROR, NULL);
    ecs_assert(data != NULL, ECS_INTERNAL_ERROR, NULL);

    ecs_column_t *columns = data->columns;
    int32_t i, column_count = table->column_count;

    if (!columns && column_count) {
        init_data(world, table, data);
        columns = data->columns;
    }

    ecs_vector_set_count(&data->entities, ecs_entity_t, count);
    ecs_vector_set_count(&data->record_ptrs, ecs_record_t*, count);

    for (i = 0; i < column_count; i ++) {
        int16_t size = columns[i].size;
        int16_t alignment = columns[i].alignment;
        if (size) {
            ecs_vector_set_count_t(&columns[i].data, size, alignment, count);
        }
    }

    table->alloc_count ++;

    return 0;
}

int32_t ecs_table_data_count(
    ecs_data_t *data)
{
    return data ? ecs_vector_count(data->entities) : 0;
}

int32_t ecs_table_count(
    ecs_table_t *table)
{
    ecs_assert(table != NULL, ECS_INTERNAL_ERROR, NULL);
    ecs_data_t *data = ecs_vector_first(table->data, ecs_data_t);
    if (!data) {
        return 0;
    }

    return ecs_table_data_count(data);
}

void ecs_table_swap(
    ecs_world_t *world,
    ecs_stage_t *stage,
    ecs_table_t *table,
    ecs_data_t *data,
    int32_t row_1,
    int32_t row_2)
{    
    (void)world;

    ecs_assert(data != NULL, ECS_INTERNAL_ERROR, NULL);
    ecs_column_t *columns = data->columns;
    ecs_assert(columns != NULL, ECS_INTERNAL_ERROR, NULL);

    ecs_assert(row_1 >= 0, ECS_INTERNAL_ERROR, NULL);
    ecs_assert(row_2 >= 0, ECS_INTERNAL_ERROR, NULL);
    
    if (row_1 == row_2) {
        return;
    }

    ecs_entity_t *entities = ecs_vector_first(data->entities, ecs_entity_t);
    ecs_entity_t e1 = entities[row_1];
    ecs_entity_t e2 = entities[row_2];

    ecs_record_t **record_ptrs = ecs_vector_first(data->record_ptrs, ecs_record_t*);
    ecs_record_t *record_ptr_1 = record_ptrs[row_1];
    ecs_record_t *record_ptr_2 = record_ptrs[row_2];
    
    /* Get pointers to records in entity index */
    if (!record_ptr_1) {
        record_ptr_1 = ecs_eis_get(stage, e1);
    }

    if (!record_ptr_2) {
        record_ptr_2 = ecs_eis_get(stage, e2);
    }

    /* Swap entities */
    entities[row_1] = e2;
    entities[row_2] = e1;
    record_ptr_1->row = row_2 + 1;
    record_ptr_2->row = row_1 + 1;
    record_ptrs[row_1] = record_ptr_2;
    record_ptrs[row_2] = record_ptr_1;

    /* Swap columns */
    int32_t i, column_count = table->column_count;
    
    for (i = 0; i < column_count; i ++) {
        int16_t size = columns[i].size;
        int16_t alignment = columns[i].alignment;
        void *ptr = ecs_vector_first_t(columns[i].data, size, alignment);

        if (size) {
            void *tmp = ecs_os_alloca(size);

            void *el_1 = ECS_OFFSET(ptr, size * row_1);
            void *el_2 = ECS_OFFSET(ptr, size * row_2);

            ecs_os_memcpy(tmp, el_1, size);
            ecs_os_memcpy(el_1, el_2, size);
            ecs_os_memcpy(el_2, tmp, size);
        }
    }

    /* If the table is monitored indicate that there has been a change */
    mark_table_dirty(table, 0);    
}

static
void merge_vector(
    ecs_vector_t **dst_out,
    ecs_vector_t *src,
    int16_t size,
    int16_t alignment)
{
    ecs_vector_t *dst = *dst_out;
    int32_t dst_count = ecs_vector_count(dst);

    if (!dst_count) {
        if (dst) {
            ecs_vector_free(dst);
        }

        *dst_out = src;
    
    /* If the new table is not empty, copy the contents from the
     * src into the dst. */
    } else {
        int32_t src_count = ecs_vector_count(src);
        ecs_vector_set_count_t(&dst, size, alignment, dst_count + src_count);
        
        void *dst_ptr = ecs_vector_first_t(dst, size, alignment);
        void *src_ptr = ecs_vector_first_t(src, size, alignment);

        dst_ptr = ECS_OFFSET(dst_ptr, size * dst_count);
        
        ecs_os_memcpy(dst_ptr, src_ptr, size * src_count);

        ecs_vector_free(src);
        *dst_out = dst;
    }
}

static
void merge_table_data(
    ecs_world_t *world,
    ecs_table_t *new_table,
    ecs_table_t *old_table,
    int32_t old_count,
    int32_t new_count,
    ecs_data_t *old_data,
    ecs_data_t *new_data)
{
    int32_t i_new, new_component_count = new_table->column_count;
    int32_t i_old = 0, old_component_count = old_table->column_count;
    ecs_entity_t *new_components = ecs_vector_first(new_table->type, ecs_entity_t);
    ecs_entity_t *old_components = ecs_vector_first(old_table->type, ecs_entity_t);

    ecs_column_t *old_columns = old_data->columns;
    ecs_column_t *new_columns = new_data->columns;

    if (!new_columns && !new_data->entities) {
        init_data(world, new_table, new_data);
        new_columns = new_data->columns;
    }

    if (!old_count) {
        return;
    }

    /* Merge entities */
    merge_vector(&new_data->entities, old_data->entities, ECS_SIZEOF(ecs_entity_t), 
        ECS_ALIGNOF(ecs_entity_t));
    old_data->entities = NULL;
    ecs_entity_t *entities = ecs_vector_first(new_data->entities, ecs_entity_t);

    ecs_assert(ecs_vector_count(new_data->entities) == old_count + new_count, 
        ECS_INTERNAL_ERROR, NULL);

    /* Merge entity index record pointers */
    merge_vector(&new_data->record_ptrs, old_data->record_ptrs, 
        ECS_SIZEOF(ecs_record_t*), ECS_ALIGNOF(ecs_record_t*));
    old_data->record_ptrs = NULL;        

    for (i_new = 0; (i_new < new_component_count) && (i_old < old_component_count); ) {
        ecs_entity_t new_component = new_components[i_new];
        ecs_entity_t old_component = old_components[i_old];
        int16_t size = new_columns[i_new].size;
        int16_t alignment = new_columns[i_new].alignment;

        if ((new_component & ECS_ROLE_MASK) || 
            (old_component & ECS_ROLE_MASK)) 
        {
            break;
        }

        if (new_component == old_component) {
            merge_vector(
                &new_columns[i_new].data, old_columns[i_old].data, size, 
                alignment);

            old_columns[i_old].data = NULL;

            /* Mark component column as dirty */
            mark_table_dirty(new_table, i_new + 1);
            
            i_new ++;
            i_old ++;
        } else if (new_component < old_component) {
            /* New column does not occur in old table, make sure vector is large
             * enough. */
            if (size) {
                ecs_column_t *column = &new_columns[i_new];
                ecs_vector_set_count_t(&column->data, size, alignment,
                    old_count + new_count);

                /* Construct new values */
                ecs_c_info_t *c_info;
                ecs_xtor_t ctor;
                if ((c_info = new_table->c_info[i_new]) && 
                    (ctor = c_info->lifecycle.ctor)) 
                {
                    ctor_component(world, new_component, column, entities, 
                        0, old_count + new_count);
                }
            }
            
            i_new ++;
        } else if (new_component > old_component) {
            if (size) {
                ecs_column_t *column = &old_columns[i_old];
                
                /* Destruct old values */
                ecs_c_info_t *c_info;
                ecs_xtor_t dtor;
                if ((c_info = old_table->c_info[i_old]) && 
                    (dtor = c_info->lifecycle.dtor)) 
                {
                    dtor_component(world, old_component, column, entities, 0, 
                        old_count);
                }

                /* Old column does not occur in new table, remove */
                ecs_vector_free(column->data);
                column->data = NULL;

                i_old ++;
            }
        }
    }

    move_switch_columns(
        new_table, new_data, new_count, old_table, old_data, 0, old_count);

    /* Initialize remaining columns */
    for (; i_new < new_component_count; i_new ++) {
        ecs_column_t *column = &new_columns[i_new];
        int16_t size = column->size;
        int16_t alignment = column->alignment;

        if (size) {
            ecs_vector_set_count_t(&column->data, size, alignment,
                old_count + new_count);

            /* Construct new values */
            ecs_c_info_t *c_info;
            ecs_xtor_t ctor;
            if ((c_info = new_table->c_info[i_new]) && 
                (ctor = c_info->lifecycle.ctor)) 
            {
                ctor_component(world, c_info->component, column, entities, 
                    0, old_count + new_count);
            }
        }
    }

    /* Destroy remaining columns */
    for (; i_old < old_component_count; i_old ++) {
        ecs_column_t *column = &old_columns[i_old];
                
        /* Destruct old values */
        ecs_c_info_t *c_info;
        ecs_xtor_t dtor;
        if ((c_info = old_table->c_info[i_old]) && 
            (dtor = c_info->lifecycle.dtor)) 
        {
            dtor_component(world, c_info->component, column, entities, 0, 
                old_count);
        }

        /* Old column does not occur in new table, remove */
        ecs_vector_free(column->data);
        column->data = NULL;
    }    

    /* Mark entity column as dirty */
    mark_table_dirty(new_table, 0); 
}

ecs_data_t* ecs_table_merge(
    ecs_world_t *world,
    ecs_table_t *new_table,
    ecs_table_t *old_table,
    ecs_data_t *new_data,
    ecs_data_t *old_data)
{
    ecs_assert(old_table != NULL, ECS_INTERNAL_ERROR, NULL);
    bool move_data = false;
    
    /* If there is nothing to merge to, just clear the old table */
    if (!new_table) {
        ecs_table_clear_data(old_table, old_data);
        return NULL;
    }

    /* If there is no data to merge, drop out */
    if (!old_data) {
        return NULL;
    }

    if (!new_data) {
        new_data = ecs_table_get_or_create_data(
            world, &world->stage, new_table);
        
        if (new_table == old_table) {
            move_data = true;
        }
    }

    ecs_entity_t *old_entities = ecs_vector_first(old_data->entities, ecs_entity_t);

    int32_t old_count = ecs_vector_count(old_data->entities);
    int32_t new_count = ecs_vector_count(new_data->entities);

    ecs_record_t **old_records = ecs_vector_first(
        old_data->record_ptrs, ecs_record_t*);

    /* First, update entity index so old entities point to new type */
    int32_t i;
    for(i = 0; i < old_count; i ++) {
        ecs_record_t *record;
        if (new_table != old_table) {
            record = old_records[i];
            if (!record) {
                record = ecs_eis_get(&world->stage, old_entities[i]);
            }
        } else {
            record = ecs_eis_get_or_create(&world->stage, old_entities[i]);
        }

        bool is_monitored = record->row < 0;
        record->row = ecs_row_to_record(new_count + i, is_monitored);
        record->table = new_table;
    }

    /* Merge table columns */
    if (move_data) {
        *new_data = *old_data;
    } else {
        merge_table_data(world, new_table, old_table, old_count, new_count, 
            old_data, new_data);
    }

    new_table->alloc_count ++;

    if (!new_count && old_count) {
        ecs_table_activate(world, new_table, NULL, true);
    }

    return new_data;
}

void ecs_table_replace_data(
    ecs_world_t *world,
    ecs_table_t *table,
    ecs_data_t *data)
{
    int32_t prev_count = 0;
    ecs_data_t *table_data = ecs_vector_first(table->data, ecs_data_t);
    ecs_assert(!data || data != table_data, ECS_INTERNAL_ERROR, NULL);

    if (table_data) {
        prev_count = ecs_vector_count(table_data->entities);
        run_remove_actions(
            world, table, table_data, 0, ecs_table_data_count(table_data), false);
        deinit_data(table, table_data);
    }

    if (data) {
        table_data = ecs_table_get_or_create_data(world, &world->stage, table);
        *table_data = *data;
    } else {
        return;
    }

    int32_t count = ecs_table_count(table);

    if (!prev_count && count) {
        ecs_table_activate(world, table, 0, true);
    } else if (prev_count && !count) {
        ecs_table_activate(world, table, 0, false);
    }
}

bool ecs_table_match_filter(
    ecs_world_t *world,
    ecs_table_t *table,
    const ecs_filter_t *filter)
{
    if (!filter) {
        return true;
    }

    ecs_type_t type = table->type;
    
    if (filter->include) {
        /* If filter kind is exact, types must be the same */
        if (filter->include_kind == EcsMatchExact) {
            if (type != filter->include) {
                return false;
            }

        /* Default for include_kind is MatchAll */
        } else if (!ecs_type_contains(world, type, filter->include, 
            filter->include_kind != EcsMatchAny, true)) 
        {
            return false;
        }
    }

    if (filter->exclude) {
        /* If filter kind is exact, types must be the same */
        if (filter->exclude_kind == EcsMatchExact) {
            if (type == filter->exclude) {
                return false;
            }
        
        /* Default for exclude_kind is MatchAny */                
        } else if (ecs_type_contains(world, type, filter->exclude, 
            filter->exclude_kind == EcsMatchAll, true))
        {
            return false;
        }
    }

    return true;
}

int32_t* ecs_table_get_dirty_state(
    ecs_table_t *table)
{
    if (!table->dirty_state) {
        table->dirty_state = ecs_os_calloc(ECS_SIZEOF(int32_t) * (table->column_count + 1));
        ecs_assert(table->dirty_state != NULL, ECS_INTERNAL_ERROR, NULL);
    }
    return table->dirty_state;
}

int32_t* ecs_table_get_monitor(
    ecs_table_t *table)
{
    int32_t *dirty_state = ecs_table_get_dirty_state(table);
    ecs_assert(dirty_state != NULL, ECS_INTERNAL_ERROR, NULL);

    int32_t column_count = table->column_count;
    return ecs_os_memdup(dirty_state, (column_count + 1) * ECS_SIZEOF(int32_t));
}

void ecs_table_notify(
    ecs_world_t *world,
    ecs_table_t *table,
    ecs_table_event_t *event)
{
    switch(event->kind) {
    case EcsTableQueryMatch:
        register_query(
            world, table, event->query, event->matched_table_index);
        break;
    case EcsTableQueryUnmatch:
        /* TODO */
        break;
    case EcsTableComponentInfo:
        notify_component_info(world, table, event->component);
        break;
    }
}
