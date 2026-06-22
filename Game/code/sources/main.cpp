#include <iostream>
#include <ECS.hpp>

// =========================================================
// Component definitions - plain data structs, no logic
// =========================================================

struct Position
{
    float x, y, z;
};

struct Velocity
{
    float x, y, z;
};

struct Health
{
    float current;
    float max;
};

struct Tag_Player {};   // tag component: no data, just marks the entity

// =========================================================
// Helper: print entity info
// =========================================================

void Print_entity(ECS::World& _world, ECS::Entity _entity)
{
    std::cout << "  Entity " << _entity << ":";

    if (auto* pos = _world.Try_get_component<Position>(_entity))
    {
        std::cout << " Position(" << pos->x << ", " << pos->y << ", " << pos->z << ")";
    }
    if (auto* vel = _world.Try_get_component<Velocity>(_entity))
    {
        std::cout << " Velocity(" << vel->x << ", " << vel->y << ", " << vel->z << ")";
    }
    if (auto* hp = _world.Try_get_component<Health>(_entity))
    {
        std::cout << " Health(" << hp->current << "/" << hp->max << ")";
    }
    if (_world.Has_component<Tag_Player>(_entity))
    {
        std::cout << " [PLAYER]";
    }

    std::cout << "\n";
}

int main()
{
    std::cout << "=== Probando ECS ===\n\n";

    ECS::World world;

    // ---------- Crear entidades ----------
    std::cout << "--- Creando entidades ---\n";

    ECS::Entity player = world.Create_entity();
    world.Add_component<Position>(player, 0.0f, 0.0f, 0.0f);
    world.Add_component<Velocity>(player, 1.0f, 0.0f, 0.0f);
    world.Add_component<Health>(player, 100.0f, 100.0f);
    world.Add_component<Tag_Player>(player);

    ECS::Entity enemy1 = world.Create_entity();
    world.Add_component<Position>(enemy1, 5.0f, 0.0f, 0.0f);
    world.Add_component<Velocity>(enemy1, -1.0f, 0.0f, 0.0f);
    world.Add_component<Health>(enemy1, 50.0f, 50.0f);

    ECS::Entity enemy2 = world.Create_entity();
    world.Add_component<Position>(enemy2, 10.0f, 0.0f, 0.0f);
    world.Add_component<Health>(enemy2, 30.0f, 30.0f);

    ECS::Entity prop = world.Create_entity();
    world.Add_component<Position>(prop, 3.0f, 0.0f, 0.0f);

    std::cout << "Entidades creadas: " << world.Entity_count() << "\n";
    std::cout << "Con Position: " << world.Entity_count_with<Position>() << "\n";
    std::cout << "Con Velocity: " << world.Entity_count_with<Velocity>() << "\n";
    std::cout << "Con Health:   " << world.Entity_count_with<Health>() << "\n\n";

    Print_entity(world, player);
    Print_entity(world, enemy1);
    Print_entity(world, enemy2);
    Print_entity(world, prop);

    // ---------- Query: mover entidades con Position y Velocity ----------
    std::cout << "\n--- Query<Velocity, Position>: simulando un frame ---\n";

    float delta_time = 0.016f;

    world.Query<Velocity, Position>([&](ECS::Entity e, Velocity& vel, Position& pos)
        {
            pos.x += vel.x * delta_time;
            pos.y += vel.y * delta_time;
            pos.z += vel.z * delta_time;

            std::cout << "  Moviendo entidad " << e
                << " -> (" << pos.x << ", " << pos.y << ", " << pos.z << ")\n";
        });

    // ---------- Each: imprimir todas las saludes ----------
    std::cout << "\n--- Each<Health>: listando saludes ---\n";

    world.Each<Health>([](ECS::Entity e, Health& hp)
        {
            std::cout << "  Entidad " << e
                << " HP: " << hp.current << "/" << hp.max << "\n";
        });

    // ---------- Try_get_component ----------
    std::cout << "\n--- Try_get_component ---\n";

    auto* player_vel = world.Try_get_component<Velocity>(player);
    auto* prop_vel = world.Try_get_component<Velocity>(prop);

    std::cout << "  Player tiene Velocity: " << (player_vel ? "si" : "no") << "\n";
    std::cout << "  Prop tiene Velocity:   " << (prop_vel ? "si" : "no") << "\n";

    // ---------- Get_or_add_component ----------
    std::cout << "\n--- Get_or_add_component ---\n";

    Velocity& prop_vel_added = world.Get_or_add_component<Velocity>(prop, 0.5f, 0.0f, 0.0f);
    std::cout << "  Prop ahora tiene Velocity: ("
        << prop_vel_added.x << ", "
        << prop_vel_added.y << ", "
        << prop_vel_added.z << ")\n";

    // ---------- Clone_entity ----------
    std::cout << "\n--- Clone_entity ---\n";

    ECS::Entity enemy1_clone = world.Clone_entity(enemy1);
    std::cout << "  Clon de enemy1 creado con ID: " << enemy1_clone << "\n";
    Print_entity(world, enemy1_clone);

    // ---------- Remove_component ----------
    std::cout << "\n--- Remove_component ---\n";

    std::cout << "  Quitando Velocity a enemy1...\n";
    world.Remove_component<Velocity>(enemy1);
    std::cout << "  enemy1 tiene Velocity: "
        << (world.Has_component<Velocity>(enemy1) ? "si" : "no") << "\n";

    // ---------- Destroy_entity ----------
    std::cout << "\n--- Destroy_entity ---\n";

    std::cout << "  Destruyendo prop...\n";
    world.Destroy_entity(prop);
    std::cout << "  Entidades vivas: " << world.Entity_count() << "\n";
    std::cout << "  prop Is_alive: "
        << (world.Is_alive(prop) ? "si" : "no") << "\n";

    // ---------- Reserve ----------
    std::cout << "\n--- Reserve (pre-reservar capacidad) ---\n";

    world.Reserve<Position>(1000);
    std::cout << "  Capacidad de Position pre-reservada para 1000 entidades.\n";

    // ---------- Clear ----------
    std::cout << "\n--- Clear ---\n";

    world.Clear();
    std::cout << "  Mundo limpiado. Entidades vivas: " << world.Entity_count() << "\n";

    std::cout << "\nECS funciona correctamente.\n";

    return 0;
}