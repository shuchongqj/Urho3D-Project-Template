#include <Urho3D/IO/Log.h>
#include <Urho3D/Core/Context.h>
#include <Urho3D/Core/CoreEvents.h>
#include <Urho3D/Engine/DebugHud.h>
#include <Urho3D/Math/Ray.h>
#include <Urho3D/Container/Vector.h>
#include <Urho3D/Graphics/Octree.h>
#include <Urho3D/IO/FileSystem.h>
#include "VoxelWorld.h"
#include "../../SceneManager.h"
#include "VoxelEvents.h"
#include "../../Console/ConsoleHandlerEvents.h"
#include "../../Global.h"
#include "LightManager.h"
#include "TreeGenerator.h"

using namespace VoxelEvents;
using namespace ConsoleHandlerEvents;

void UpdateChunkState(const WorkItem* item, unsigned threadIndex)
{
    Timer loadTime;
    VoxelWorld* world = reinterpret_cast<VoxelWorld*>(item->aux_);
    MutexLock lock(world->mutex_);
    if (world->GetSubsystem<LightManager>()) {
        world->GetSubsystem<LightManager>()->Process();
    }
    if (world->GetSubsystem<TreeGenerator>()) {
        world->GetSubsystem<TreeGenerator>()->Process();
    }
    if (world->GetSubsystem<DebugHud>()) {
        world->GetSubsystem<DebugHud>()->SetAppStats("Chunks Loaded", world->chunks_.Size());
    }

    int counter = 0;
    for (auto chIt = world->chunks_.Begin(); chIt != world->chunks_.End(); ++chIt) {
        if ((*chIt).second_ && (*chIt).second_->IsActive()) {
            counter++;
        }
    }
    if (world->GetSubsystem<DebugHud>()) {
        world->GetSubsystem<DebugHud>()->SetAppStats("Active chunks", counter);
    }

    for (auto it = world->chunks_.Begin(); it != world->chunks_.End(); ++it) {
        if (!(*it).second_) {
            continue;
        }

        if (world->reloadAllChunks_) {
            (*it).second_->MarkForGeometryCalculation();
        }
        // Initialize new chunks
        if (!(*it).second_->IsLoaded()) {
            (*it).second_->Load();
            for (int i = 0; i < 6; i++) {
                auto neighbor = (*it).second_->GetNeighbor(static_cast<BlockSide>(i));
                if (neighbor) {
                    neighbor->MarkForGeometryCalculation();
                }
            }
        }

        if (!(*it).second_->IsGeometryCalculated()) {
            (*it).second_->CalculateGeometry();
//            URHO3D_LOGINFO("CalculateGeometry " + (*it).second_->GetPosition().ToString());
        }
    }

    world->reloadAllChunks_ = false;
//    URHO3D_LOGINFO("Chunks updated in " + String(loadTime.GetMSec(false)) + "ms");
}

VoxelWorld::VoxelWorld(Context* context):
    Object(context)
{
}

void VoxelWorld::Init()
{
    scene_ = GetSubsystem<SceneManager>()->GetActiveScene();

    SubscribeToEvent(E_UPDATE, URHO3D_HANDLER(VoxelWorld, HandleUpdate));
    SubscribeToEvent(E_WORKITEMCOMPLETED, URHO3D_HANDLER(VoxelWorld, HandleWorkItemFinished));

    SendEvent(
            E_CONSOLE_COMMAND_ADD,
            ConsoleCommandAdd::P_NAME, "chunk_visible_distance",
            ConsoleCommandAdd::P_EVENT, "#chunk_visible_distance",
            ConsoleCommandAdd::P_DESCRIPTION, "How far away the generated chunks are visible",
            ConsoleCommandAdd::P_OVERWRITE, true
    );
    SubscribeToEvent("#chunk_visible_distance", [&](StringHash eventType, VariantMap& eventData) {
        StringVector params = eventData["Parameters"].GetStringVector();
        if (params.Size() != 2) {
            URHO3D_LOGERROR("radius parameter is required!");
            return;
        }
        int value = ToInt(params[1]);
        visibleDistance_ = value;
        URHO3D_LOGINFOF("Changing chunk visibility radius to %d", value);
    });

    SendEvent(
            E_CONSOLE_COMMAND_ADD,
            ConsoleCommandAdd::P_NAME, "world_reset",
            ConsoleCommandAdd::P_EVENT, "#world_reset",
            ConsoleCommandAdd::P_DESCRIPTION, "Remove saved chunks",
            ConsoleCommandAdd::P_OVERWRITE, true
    );
    SubscribeToEvent("#world_reset", [&](StringHash eventType, VariantMap& eventData) {
        StringVector params = eventData["Parameters"].GetStringVector();
        if (params.Size() != 1) {
            URHO3D_LOGERROR("Thi command doesn't have any arguments!");
            return;
        }
        if(GetSubsystem<FileSystem>()->DirExists("World")) {
            Vector<String> files;
            GetSubsystem<FileSystem>()->ScanDir(files, "World", "", SCAN_FILES, false);
            for (auto it = files.Begin(); it != files.End(); ++it) {
                URHO3D_LOGINFO("Deleting file " + (*it));
                GetSubsystem<FileSystem>()->Delete("World/" + (*it));
            }
        }
    });

    SendEvent(
            E_CONSOLE_COMMAND_ADD,
            ConsoleCommandAdd::P_NAME, "sunlight",
            ConsoleCommandAdd::P_EVENT, "#sunlight",
            ConsoleCommandAdd::P_DESCRIPTION, "Set sunlight level [0-15]",
            ConsoleCommandAdd::P_OVERWRITE, true
    );
    SubscribeToEvent("#sunlight", [&](StringHash eventType, VariantMap& eventData) {
        StringVector params = eventData["Parameters"].GetStringVector();
        if (params.Size() != 2) {
            URHO3D_LOGERROR("Thi command requires exactly 1 argument!");
            return;
        }
        int level = ToInt(params[1]);
        Chunk::sunlightLevel = level;
        reloadAllChunks_ = true;
    });
}

void VoxelWorld::RegisterObject(Context* context)
{
    context->RegisterFactory<VoxelWorld>();
}

void VoxelWorld::AddObserver(SharedPtr<Node> observer)
{
    observers_.Push(observer);
    URHO3D_LOGINFO("Adding observer to voxel world!");
}

void VoxelWorld::RemoveObserver(SharedPtr<Node> observer)
{
    auto it = observers_.Find(observer);
    if (it != observers_.End()) {
        observers_.Erase(it);
        URHO3D_LOGINFO("Removing observer from voxel world!");
    }
}

void VoxelWorld::HandleUpdate(StringHash eventType, VariantMap& eventData)
{
    int loadedChunkCounter = 0;
    if (!removeBlocks_.Empty()) {
        auto chunk = GetChunkByPosition(removeBlocks_.Front());
        if (chunk) {
            VariantMap& data = GetEventDataMap();
            data["Position"] = removeBlocks_.Front();
            data["ControllerId"] = -1;
            if (chunk->GetNode()) {
                chunk->GetNode()->SendEvent("ChunkHit", data);
                removeBlocks_.PopFront();
            }
        }
    }

    UpdateChunks();
}

Chunk* VoxelWorld::CreateChunk(const Vector3& position)
{
    String id = GetChunkIdentificator(position);
    chunks_[id] = new Chunk(context_);
    chunks_[id]->Init(scene_, position);
    return chunks_[id].Get();
}

Vector3 VoxelWorld::GetNodeToChunkPosition(Node* node)
{
    Vector3 position = node->GetWorldPosition();
    return GetWorldToChunkPosition(position);
}

bool VoxelWorld::IsChunkLoaded(const Vector3& position)
{
    String id = GetChunkIdentificator(position);
    if (chunks_.Contains(id) && chunks_[id]) {
        return true;
    }
    return false;
}

void VoxelWorld::LoadChunk(const Vector3& position)
{
//    Vector3 fixedChunkPosition = GetWorldToChunkPosition(position);
//    if (!IsChunkLoaded(fixedChunkPosition)) {
//        AddChunkToQueue(position);
//    }
//    Vector<Vector3> positions;


    //Terrain blocks only
//    for (int x = -5; x < 5; x++) {
//        for (int z = -5; z < 5; z++) {
//            Vector3 terrain = Vector3(fixedChunkPosition + Vector3::FORWARD * SIZE_Z * z + Vector3::LEFT * SIZE_X * x);
//            terrain.y_ = GetSubsystem<ChunkGenerator>()->GetTerrainHeight(terrain);
//            positions.Push(terrain);
////            positions.Push(terrain + Vector3::UP * SIZE_Y);
//            positions.Push(terrain + Vector3::DOWN * SIZE_Y);
//        }
//    }
//
//    // Same
//    positions.Push(Vector3(fixedChunkPosition + Vector3::LEFT * SIZE_X));
//    positions.Push(Vector3(fixedChunkPosition + Vector3::RIGHT * SIZE_X));
//    positions.Push(Vector3(fixedChunkPosition + Vector3::FORWARD * SIZE_Z));
//    positions.Push(Vector3(fixedChunkPosition + Vector3::BACK * SIZE_Z));
//
//    positions.Push(Vector3(fixedChunkPosition + Vector3::BACK * SIZE_Z + Vector3::LEFT * SIZE_X));
//    positions.Push(Vector3(fixedChunkPosition + Vector3::BACK * SIZE_Z + Vector3::RIGHT * SIZE_X));
//    positions.Push(Vector3(fixedChunkPosition + Vector3::FORWARD * SIZE_Z + Vector3::LEFT * SIZE_X));
//    positions.Push(Vector3(fixedChunkPosition + Vector3::FORWARD * SIZE_Z + Vector3::RIGHT * SIZE_X));
//
//    // Up
//    positions.Push(Vector3(fixedChunkPosition + Vector3::UP * SIZE_Y + Vector3::LEFT * SIZE_X));
//    positions.Push(Vector3(fixedChunkPosition + Vector3::UP * SIZE_Y + Vector3::RIGHT * SIZE_X));
//    positions.Push(Vector3(fixedChunkPosition + Vector3::UP * SIZE_Y + Vector3::FORWARD * SIZE_Z));
//    positions.Push(Vector3(fixedChunkPosition + Vector3::UP * SIZE_Y + Vector3::BACK * SIZE_Z));
//    positions.Push(Vector3(fixedChunkPosition + Vector3::UP * SIZE_Y));
//
//    positions.Push(Vector3(fixedChunkPosition + Vector3::UP * SIZE_Y + Vector3::BACK * SIZE_Z + Vector3::LEFT * SIZE_X));
//    positions.Push(Vector3(fixedChunkPosition + Vector3::UP * SIZE_Y + Vector3::BACK * SIZE_Z + Vector3::RIGHT * SIZE_X));
//    positions.Push(Vector3(fixedChunkPosition + Vector3::UP * SIZE_Y + Vector3::FORWARD * SIZE_Z + Vector3::LEFT * SIZE_X));
//    positions.Push(Vector3(fixedChunkPosition + Vector3::UP * SIZE_Y + Vector3::FORWARD * SIZE_Z + Vector3::RIGHT * SIZE_X));
//
//    // Down
//    positions.Push(Vector3(fixedChunkPosition + Vector3::DOWN * SIZE_Y + Vector3::LEFT * SIZE_X));
//    positions.Push(Vector3(fixedChunkPosition + Vector3::DOWN * SIZE_Y + Vector3::RIGHT * SIZE_X));
//    positions.Push(Vector3(fixedChunkPosition + Vector3::DOWN * SIZE_Y + Vector3::FORWARD * SIZE_Z));
//    positions.Push(Vector3(fixedChunkPosition + Vector3::DOWN * SIZE_Y + Vector3::BACK * SIZE_Z));
//    positions.Push(Vector3(fixedChunkPosition + Vector3::DOWN * SIZE_Y));
//
//    positions.Push(Vector3(fixedChunkPosition + Vector3::DOWN * SIZE_Y + Vector3::BACK * SIZE_Z + Vector3::LEFT * SIZE_X));
//    positions.Push(Vector3(fixedChunkPosition + Vector3::DOWN * SIZE_Y + Vector3::BACK * SIZE_Z + Vector3::RIGHT * SIZE_X));
//    positions.Push(Vector3(fixedChunkPosition + Vector3::DOWN * SIZE_Y + Vector3::FORWARD * SIZE_Z + Vector3::LEFT * SIZE_X));
//    positions.Push(Vector3(fixedChunkPosition + Vector3::DOWN * SIZE_Y + Vector3::FORWARD * SIZE_Z + Vector3::RIGHT * SIZE_X));
//
//    for (auto it = positions.Begin(); it != positions.End(); ++it) {
//        if(!IsChunkLoaded((*it))) {
//            CreateChunk((*it));
//        }
//    }
}

bool VoxelWorld::IsEqualPositions(Vector3 a, Vector3 b)
{
    return Floor(a.x_) == Floor(b.x_) && Floor(a.y_) == Floor(b.y_) && Floor(a.z_) == Floor(b.z_);
}

Chunk* VoxelWorld::GetChunkByPosition(const Vector3& position)
{
    Vector3 fixedPositon = GetWorldToChunkPosition(position);
    String id = GetChunkIdentificator(fixedPositon);
    if (chunks_.Find(id) != chunks_.End() && chunks_[id]) {
        return chunks_[id].Get();
    }

    return nullptr;
}

Vector3 VoxelWorld::GetWorldToChunkPosition(const Vector3& position)
{
    return Vector3(
            Floor(position.x_ / SIZE_X) * SIZE_X,
        Floor(position.y_ / SIZE_Y) * SIZE_Y,
            Floor(position.z_ / SIZE_Z) * SIZE_Z
    );
}

IntVector3 VoxelWorld::GetWorldToChunkBlockPosition(const Vector3& position)
{
    Vector3 chunkPosition = GetWorldToChunkPosition(position);
    IntVector3 blockPosition;
    blockPosition.x_ = Floor(position.x_ - chunkPosition.x_);
    blockPosition.y_ = Floor(position.y_ - chunkPosition.y_);
    blockPosition.z_ = Floor(position.z_ - chunkPosition.z_);
    return blockPosition;
}

void VoxelWorld::RemoveBlockAtPosition(const Vector3& position)
{
    removeBlocks_.Push(position);
}

void VoxelWorld::UpdateChunks()
{
    if (!updateWorkItem_) {
        if (updateTimer_.GetMSec(false) > 1000 || chunks_.Empty()) {
            updateTimer_.Reset();
            for (auto it = chunks_.Begin(); it != chunks_.End(); ++it) {
                if ((*it).second_) {
                    (*it).second_->MarkForDeletion(true);
                    (*it).second_->SetDistance(-1);
                }
            }

            ProcessQueue();

            for (auto it = chunksToLoad_.Begin(); it != chunksToLoad_.End(); ++it) {
                Vector3 position = (*it).first_;
                String id = GetChunkIdentificator(position);
                auto chunkIterator = chunks_.Find(id);
                if (chunkIterator != chunks_.End()) {
                    (*chunkIterator).second_->MarkForDeletion(false);
                    (*chunkIterator).second_->SetDistance((*it).second_);
                } else {
                    auto chunk = CreateChunk(position);
                    chunk->SetDistance((*it).second_);
                }
            }

            chunksToLoad_.Clear();

            MutexLock lock(mutex_);
            for (auto it = chunks_.Begin(); it != chunks_.End(); ++it) {
                if ((*it).second_) {
                    if ((*it).second_->IsMarkedForDeletion()) {
                        int distance = (*it).second_->GetDistance();
//                        URHO3D_LOGINFOF("Deleting chunk distance=%d ", distance);
                        it = chunks_.Erase(it);
                    }
                    if (chunks_.End() == it) {
                        break;
                    }
                }
            }
        }


        WorkQueue *workQueue = GetSubsystem<WorkQueue>();
        updateWorkItem_ = workQueue->GetFreeItem();
        updateWorkItem_->priority_ = M_MAX_INT;
        updateWorkItem_->workFunction_ = UpdateChunkState;
        updateWorkItem_->aux_ = this;
        updateWorkItem_->sendEvent_ = true;
        updateWorkItem_->start_ = nullptr;
        updateWorkItem_->end_ = nullptr;
        workQueue->AddWorkItem(updateWorkItem_);
    }

    int renderedChunkCount = 0;
    int renderedChunkLimit = 4;
    for (auto it = chunks_.Begin(); it != chunks_.End(); ++it) {
        if ((*it).second_->ShouldRender()) {
            bool rendered = (*it).second_->Render();
//            URHO3D_LOGINFO("Rendering chunk " + (*it).second_->GetPosition().ToString());
            if (rendered) {
                renderedChunkCount++;
            }
            if (renderedChunkCount > renderedChunkLimit) {
                break;
            }
        }
    }
}

String VoxelWorld::GetChunkIdentificator(const Vector3& position)
{
    return String((int)position.x_) + "_" +  String((int)position.y_) + "_" + String((int)position.z_);
}

VoxelBlock* VoxelWorld::GetBlockAt(Vector3 position)
{
    Vector3 chunkPosition = GetWorldToChunkPosition(position);
    String id = GetChunkIdentificator(chunkPosition);
    if (chunks_.Contains(id) && chunks_[id]) {
        Vector3 blockPosition = position - chunkPosition;
        return chunks_[id]->GetBlockAt(IntVector3(blockPosition.x_, blockPosition.y_, blockPosition.z_));
    }
    return nullptr;
}

bool VoxelWorld::IsChunkValid(Chunk* chunk)
{
    for (auto it = chunks_.Begin(); it != chunks_.End(); ++it) {
        if ((*it).second_.Get() == chunk) {
            return true;
        }
    }

    return false;
}

void VoxelWorld::HandleWorkItemFinished(StringHash eventType, VariantMap& eventData) {
    using namespace WorkItemCompleted;
    WorkItem *workItem = reinterpret_cast<WorkItem *>(eventData[P_ITEM].GetPtr());
    if (workItem->aux_ != this) {
        return;
    }
//    if (workItem->workFunction_ == GenerateVoxelData) {
//        CreateNode();
//        generateWorkItem_.Reset();
//        MarkDirty();
//    } else if (workItem->workFunction_ == GenerateVertices) {
////        GetSubsystem<VoxelWorld>()->AddChunkToRenderQueue(this);
//        Render();
//        generateGeometryWorkItem_.Reset();
//    } else
    if (workItem->workFunction_ == UpdateChunkState) {
        updateWorkItem_.Reset();
    }
}

const String VoxelWorld::GetBlockName(BlockType type)
{
    switch (type) {
        case BT_AIR:
            return "BT_AIR";
        case BT_STONE:
            return "BT_STONE";
        case BT_DIRT:
            return "BT_DIRT";
        case BT_SAND:
            return "BT_SAND";
        case BT_COAL:
            return "BT_COAL";
        case BT_TORCH:
            return "BT_TORCH";
        case BT_WOOD:
            return "BT_WOOD";
        case BT_TREE_LEAVES:
            return "BT_TREE_LEAVES";
        case BT_WATER:
            return "BT_WATER";
        default:
            return "BT_NONE";
    }
}

void VoxelWorld::ProcessQueue()
{
    for (auto it = observers_.Begin(); it != observers_.End(); ++it) {
        AddChunkToQueue(((*it)->GetPosition()));
    }

    while (!chunkBfsQueue_.empty()) {
        ChunkNode& node = chunkBfsQueue_.front();
        int distance = node.distance_ + 1;
        chunkBfsQueue_.pop();
        if (distance < visibleDistance_) {
            AddChunkToQueue(node.position_ + Vector3::LEFT * SIZE_X, distance);
            AddChunkToQueue(node.position_ + Vector3::RIGHT * SIZE_X, distance);
            AddChunkToQueue(node.position_ + Vector3::FORWARD * SIZE_Z, distance);
            AddChunkToQueue(node.position_ + Vector3::BACK * SIZE_Z, distance);
            AddChunkToQueue(node.position_ + Vector3::UP * SIZE_Y, distance);
            AddChunkToQueue(node.position_ + Vector3::DOWN * SIZE_Y, distance);
        }
    }
}

void VoxelWorld::AddChunkToQueue(Vector3 position, int distance)
{
    Vector3 fixedPosition = GetWorldToChunkPosition(position);
    String id = GetChunkIdentificator(fixedPosition);
    ChunkNode node(position, distance);
    if (!chunksToLoad_.Contains(fixedPosition)) {
        chunksToLoad_[fixedPosition] = distance;
        chunkBfsQueue_.emplace(node);
    }
}