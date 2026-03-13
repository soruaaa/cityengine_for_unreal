/* Copyright 2024 Esri
 *
 * Licensed under the Apache License Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "VitruvioBatchActor.h"

#include "Materials/Material.h"
#include "Runtime/CoreUObject/Public/UObject/ConstructorHelpers.h"
#include "GenerateCompletedCallbackProxy.h"

void UTile::MarkForAttributeEvaluation(UVitruvioComponent* VitruvioComponent, UGenerateCompletedCallbackProxy* CallbackProxy)
{
	bMarkedForEvaluateAttributes = true;
	if (CallbackProxy)
	{
		EvaluateAttributesCallbackProxies.Add(VitruvioComponent, CallbackProxy);
	}
}

void UTile::UnmarkForAttributeEvaluation()
{
	bMarkedForEvaluateAttributes = false;
}

void UTile::MarkForGenerate(UVitruvioComponent* VitruvioComponent, UGenerateCompletedCallbackProxy* CallbackProxy)
{
	if (bMarkedForEvaluateAttributes)
	{
		UnmarkForAttributeEvaluation();
	}
	
	bMarkedForGenerate = true;
	if (CallbackProxy)
	{
		GenerateCallbackProxies.Add(VitruvioComponent, CallbackProxy);
	}
}

void UTile::UnmarkForGenerate()
{
	bMarkedForGenerate = false;
}

void UTile::Add(UVitruvioComponent* VitruvioComponent)
{
	VitruvioComponents.Add(VitruvioComponent);
}

void UTile::Remove(UVitruvioComponent* VitruvioComponent)
{
	VitruvioComponents.Remove(VitruvioComponent);
}

bool UTile::Contains(UVitruvioComponent* VitruvioComponent) const
{
	return VitruvioComponents.Contains(VitruvioComponent);
}

TTuple<TArray<FInitialShape>, TArray<UVitruvioComponent*>> UTile::GetInitialShapes(const TFunction<bool(UVitruvioComponent*)>& Filter)
{
	TArray<FInitialShape> InitialShapes;
	TArray<UVitruvioComponent*> ValidVitruvioComponents;
	
	for (UVitruvioComponent* VitruvioComponent : VitruvioComponents)
	{
		if (!VitruvioComponent->HasValidInputData() || !Filter(VitruvioComponent))
		{
			continue;
		}

		ValidVitruvioComponents.Add(VitruvioComponent);
		InitialShapes.Add(VitruvioComponent->GetInitialShape());
	}

	return MakeTuple(MoveTemp(InitialShapes), ValidVitruvioComponents);
}

TTuple<TArray<FInitialShape>, TArray<UVitruvioComponent*>> UTile::GetInitialShapes()
{
	return GetInitialShapes([](UVitruvioComponent*) { return true; });
}

void FGrid::MarkForAttributeEvaluation(UVitruvioComponent* VitruvioComponent, UGenerateCompletedCallbackProxy* CallbackProxy)
{
	if (UTile** FoundTile = TilesByComponent.Find(VitruvioComponent))
	{
		UTile* Tile = *FoundTile;
		Tile->MarkForAttributeEvaluation(VitruvioComponent, CallbackProxy);
	}
}

void FGrid::MarkAllForAttributeEvaluation()
{
	for (const auto& [Component, Tile] : TilesByComponent)
	{
		Tile->MarkForGenerate(Component);
	}
}

void FGrid::MarkForGenerate(UVitruvioComponent* VitruvioComponent, UGenerateCompletedCallbackProxy* CallbackProxy)
{
	if (UTile** FoundTile = TilesByComponent.Find(VitruvioComponent))
	{
		UTile* Tile = *FoundTile;
		Tile->MarkForGenerate(VitruvioComponent, CallbackProxy);
	}
}

void FGrid::MarkAllForGenerate()
{
	for (const auto& [Component, Tile] : TilesByComponent)
	{
		Tile->MarkForGenerate(Component);
	}
}

void FGrid::RegisterAll(const TSet<UVitruvioComponent*>& VitruvioComponents, AVitruvioBatchActor* VitruvioBatchActor, bool bGeneateModel)
{
	for (UVitruvioComponent* VitruvioComponent : VitruvioComponents)
	{
		Register(VitruvioComponent, VitruvioBatchActor, bGeneateModel);
	} 
}

void FGrid::Register(UVitruvioComponent* VitruvioComponent, AVitruvioBatchActor* VitruvioBatchActor, bool bGenerateModel)
{
	const FIntPoint Position = VitruvioBatchActor->GetPosition(VitruvioComponent);
	
	UTile* Tile;
	if (UTile** TileResult = Tiles.Find(Position))
	{
		Tile = *TileResult;
	}
	else
	{
		Tile = NewObject<UTile>();
		Tile->Location = Position;
		Tiles.Add(Position, Tile);
	}

	if (!Tile->Contains(VitruvioComponent))
	{
		Tile->Add(VitruvioComponent);
		if (bGenerateModel)
		{
			Tile->MarkForGenerate(VitruvioComponent);
		}
		TilesByComponent.Add(VitruvioComponent, Tile);
	}
}

void FGrid::Unregister(UVitruvioComponent* VitruvioComponent)
{
	if (UTile** FoundTile = TilesByComponent.Find(VitruvioComponent))
	{
		UTile* Tile = *FoundTile;

		if (Tile->GenerateToken)
		{
			Tile->GenerateToken->Invalidate();
			Tile->GenerateToken.Reset();
		}
		
		Tile->Remove(VitruvioComponent);
		Tile->MarkForGenerate(VitruvioComponent);
	}
}

void FGrid::Clear()
{
	for (auto& [VitruvioComponent, Tile] : TilesByComponent)
	{
		if (Tile->GeneratedModelComponent && IsValid(Tile->GeneratedModelComponent))
		{
			TArray<USceneComponent*> InstanceSceneComponents;
			Tile->GeneratedModelComponent->GetChildrenComponents(true, InstanceSceneComponents);
			for (USceneComponent* InstanceComponent : InstanceSceneComponents)
			{
				InstanceComponent->DestroyComponent(true);
			}
			
			Tile->GeneratedModelComponent->DestroyComponent(true);
		}
	}

	TilesByComponent.Reset();
	Tiles.Reset();
}

TArray<UTile*> FGrid::GetTilesMarkedForGenerate() const
{
	TArray<UTile*> TilesToGenerate;
	Tiles.GenerateValueArray(TilesToGenerate);

	return TilesToGenerate.FilterByPredicate([](const UTile* Tile)
	{
		return Tile->bMarkedForGenerate;
	});
}

TArray<UTile*> FGrid::GetTilesMarkedForAttributeEvaluation() const
{
	TArray<UTile*> TilesToGenerate;
	Tiles.GenerateValueArray(TilesToGenerate);

	return TilesToGenerate.FilterByPredicate([](const UTile* Tile)
	{
		return Tile->bMarkedForEvaluateAttributes;
	});
}

void FGrid::UnmarkAllForGenerate()
{
	for (auto& [Point, Tile] : Tiles)
	{
		Tile->UnmarkForGenerate();
	}
}

void FGrid::UnmarkAllForAttributeEvaluation()
{
	for (auto& [Point, Tile] : Tiles)
	{
		Tile->UnmarkForAttributeEvaluation();
	}
}

TArray<FInitialShape> FGrid::GetNeighboringShapes(const UTile* Tile, const TArray<FInitialShape>& InitialShapes)
{
	TArray<FInitialShape> NeighboringShapes;
	
	const TArray Directions = {
		FIntPoint(-1, 0), FIntPoint(1, 0), FIntPoint(0, -1), FIntPoint(0, 1),
		FIntPoint(-1, -1), FIntPoint(-1, 1), FIntPoint(1, -1), FIntPoint(1, 1)
	};

	for (const FIntPoint& Dir : Directions)
	{
		FIntPoint NeighborPos = Tile->Location + Dir;
		UTile** NeighborTilePtr = Tiles.Find(NeighborPos);
		if (!NeighborTilePtr)
		{
			continue;
		}
			
		UTile* NeighborTile = *NeighborTilePtr;
		auto [NeighborInitialShapes, _] = NeighborTile->GetInitialShapes([&InitialShapes](UVitruvioComponent* VitruvioComponent)
		{
			FVector Position = VitruvioComponent->GetOwner()->GetTransform().GetLocation();
			for (const FInitialShape& InputShape : InitialShapes)
			{
				if (FVector::Dist(Position, InputShape.Position) < CVarInterOcclusionNeighborQueryDistance.GetValueOnAnyThread())
				{
					return true;
				}
			}

			return false;
		});

		for (FInitialShape& NeighborShape : NeighborInitialShapes)
		{
			NeighborShape.bOccluderOnly = true;
		}
		
		NeighboringShapes.Append(MoveTemp(NeighborInitialShapes));
	}

	return NeighboringShapes;
}

AVitruvioBatchActor::AVitruvioBatchActor()
{
	SetTickGroup(TG_LastDemotable);
	PrimaryActorTick.bCanEverTick = true;

	static ConstructorHelpers::FObjectFinder<UMaterial> Opaque(TEXT("Material'/Vitruvio/Materials/M_OpaqueParent.M_OpaqueParent'"));
	static ConstructorHelpers::FObjectFinder<UMaterial> Masked(TEXT("Material'/Vitruvio/Materials/M_MaskedParent.M_MaskedParent'"));
	static ConstructorHelpers::FObjectFinder<UMaterial> Translucent(TEXT("Material'/Vitruvio/Materials/M_TranslucentParent.M_TranslucentParent'"));
	OpaqueParent = Opaque.Object;
	MaskedParent = Masked.Object;
	TranslucentParent = Translucent.Object;
	
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

#if WITH_EDITORONLY_DATA
	bLockLocation = true;
	bActorLabelEditable = false;
#endif // WITH_EDITORONLY_DATA
}

FIntPoint AVitruvioBatchActor::GetPosition(const UVitruvioComponent* VitruvioComponent) const
{
	const FVector Position = VitruvioComponent->GetOwner()->GetTransform().GetLocation();
	const int PositionX = static_cast<int>(FMath::Floor(Position.X / GridDimension.X));
	const int PositionY = static_cast<int>(FMath::Floor(Position.Y / GridDimension.Y));
	return FIntPoint {PositionX, PositionY};
}



void AVitruvioBatchActor::ProcessTiles()
{
	for (UTile* Tile : Grid.GetTilesMarkedForGenerate())
	{
		// Initialize and cleanup the model component
		UGeneratedModelStaticMeshComponent* VitruvioModelComponent = Tile->GeneratedModelComponent;
		if (VitruvioModelComponent)
		{
			VitruvioModelComponent->SetStaticMesh(nullptr);

			// Cleanup old hierarchical instances
			TArray<USceneComponent*> InstanceSceneComponents;
			VitruvioModelComponent->GetChildrenComponents(true, InstanceSceneComponents);
			for (USceneComponent* InstanceComponent : InstanceSceneComponents)
			{
				InstanceComponent->DestroyComponent(true);
			}
		}
		else
		{
			const FString TileName = FString::FromInt(NumModelComponents++);
			VitruvioModelComponent = NewObject<UGeneratedModelStaticMeshComponent>(RootComponent, FName(TEXT("GeneratedModel") + TileName),
																			   RF_Transient | RF_TextExportTransient | RF_DuplicateTransient);
			VitruvioModelComponent->CreationMethod = EComponentCreationMethod::Instance;
			RootComponent->GetOwner()->AddOwnedComponent(VitruvioModelComponent);
			VitruvioModelComponent->AttachToComponent(RootComponent, FAttachmentTransformRules::KeepRelativeTransform);
			VitruvioModelComponent->OnComponentCreated();
			VitruvioModelComponent->RegisterComponent();

			Tile->GeneratedModelComponent = VitruvioModelComponent;
		}

		auto [InitialShapes, InitialShapeVitruvioComponents] = Tile->GetInitialShapes();
		if (!InitialShapes.IsEmpty())
		{
			if (Tile->EvalAttributesToken)
			{
				Tile->EvalAttributesToken->Invalidate();
			}
			if (Tile->GenerateToken)
			{
				Tile->GenerateToken->Invalidate();
			}

			TArray<FInitialShape> OccluderOnlyShapes;
			if (bEnableOcclusionQueries)
			{
				OccluderOnlyShapes = Grid.GetNeighboringShapes(Tile, InitialShapes);
			}
			
			FBatchGenerateResult GenerateResult = VitruvioModule::Get().BatchGenerateAsync(MoveTemp(InitialShapes), bEnableOcclusionQueries, MoveTemp(OccluderOnlyShapes));
			
			Tile->GenerateToken = GenerateResult.Token;
			Tile->bIsGenerating = true;
		
			// clang-format off
			GenerateResult.Result.Next([WeakThis = MakeWeakObjectPtr(this), Tile, InitialShapeVitruvioComponents](const FBatchGenerateResult::ResultType& Result)
			{
				if (!WeakThis.IsValid())
				{
					return;
				}
				
				FScopeLock Lock(&Result.Token->Lock);

				if (Result.Token->IsInvalid())
				{
					return;
				}

				Tile->GenerateToken.Reset();

				FScopeLock QueueLock(&WeakThis->ProcessGenerateQueueCriticalSection);
				WeakThis->GenerateQueue.Enqueue({Result.Value, Tile, InitialShapeVitruvioComponents});
			});
			// clang-format on
		}
	}

	for (UTile* Tile : Grid.GetTilesMarkedForAttributeEvaluation())
	{
		auto [InitialShapes, InitialShapeVitruvioComponents] = Tile->GetInitialShapes();
		if (!InitialShapes.IsEmpty())
		{
			if (Tile->EvalAttributesToken)
			{
				Tile->EvalAttributesToken->Invalidate();
			}
			
			FAttributeMapsResult AttributeMapsResult = VitruvioModule::Get().BatchEvaluateRuleAttributesAsync(MoveTemp(InitialShapes));
			
			Tile->EvalAttributesToken = AttributeMapsResult.Token;
			Tile->bIsEvaluatingAttributes = true;

			AttributeMapsResult.Result.Next([WeakThis = MakeWeakObjectPtr(this), Tile, InitialShapeVitruvioComponents](const FAttributeMapsResult::ResultType& Result)
			{
				if (!WeakThis.IsValid())
				{
					return;
				}
				
				FScopeLock Lock(&Result.Token->Lock);

				if (Result.Token->IsInvalid())
				{
					return;
				}

				Tile->EvalAttributesToken.Reset();

				FScopeLock QueueLock(&WeakThis->ProcessAttributeEvaluationQueueCriticalSection);
				WeakThis->AttributeEvaluationQueue.Enqueue({Result.Value, Tile, InitialShapeVitruvioComponents});
			});
		}
	}
	
	Grid.UnmarkAllForGenerate();
	Grid.UnmarkAllForAttributeEvaluation();
}

void AVitruvioBatchActor::ProcessGenerateQueue()
{
	ProcessGenerateQueueCriticalSection.Lock();
	
	if (!GenerateQueue.IsEmpty())
	{
		FBatchGenerateQueueItem Item;
		GenerateQueue.Dequeue(Item);

		ProcessGenerateQueueCriticalSection.Unlock();

		if (Item.GenerateResultDescription.EvaluatedAttributes.Num() ==  Item.VitruvioComponents.Num())
		{
			for (int ComponentIndex = 0; ComponentIndex < Item.VitruvioComponents.Num(); ++ComponentIndex)
			{
				UVitruvioComponent* VitruvioComponent = Item.VitruvioComponents[ComponentIndex];
				Item.GenerateResultDescription.EvaluatedAttributes[ComponentIndex]->UpdateUnrealAttributeMap(VitruvioComponent->Attributes, VitruvioComponent);
				VitruvioComponent->bAttributesReady = true;
				VitruvioComponent->NotifyAttributesChanged();
			}
		}

		UGeneratedModelStaticMeshComponent* VitruvioModelComponent = Item.Tile->GeneratedModelComponent;

		const FConvertedGenerateResult ConvertedResult = BuildGenerateResult(Item.GenerateResultDescription,
	VitruvioModule::Get().GetMaterialCache(), VitruvioModule::Get().GetTextureCache(),
				MaterialIdentifiers, UniqueMaterialIdentifiers, OpaqueParent, MaskedParent, TranslucentParent, GetWorld());

		if (ConvertedResult.ShapeMesh)
		{
			VitruvioModelComponent->SetStaticMesh(ConvertedResult.ShapeMesh->GetStaticMesh());
			
			// Reset Material replacements
			for (int32 MaterialIndex = 0; MaterialIndex < VitruvioModelComponent->GetNumMaterials(); ++MaterialIndex)
			{
				VitruvioModelComponent->SetMaterial(MaterialIndex, VitruvioModelComponent->GetStaticMesh()->GetMaterial(MaterialIndex));
			}

			ApplyMaterialReplacements(VitruvioModelComponent, MaterialIdentifiers, MaterialReplacement);
		}

		// Cleanup old hierarchical instances
		TArray<USceneComponent*> ChildInstanceComponents;
		VitruvioModelComponent->GetChildrenComponents(true, ChildInstanceComponents);
		for (USceneComponent* InstanceComponent : ChildInstanceComponents)
		{
			InstanceComponent->DestroyComponent(true);
		}

		TMap<FString, int32> NameMap;
		TSet<FInstance> Replaced = ApplyInstanceReplacements(VitruvioModelComponent, ConvertedResult.Instances, InstanceReplacement, NameMap);
		for (const FInstance& Instance : ConvertedResult.Instances)
		{
			if (Replaced.Contains(Instance))
			{
				continue;
			}

			FString UniqueName = UniqueComponentName(Instance.Name, NameMap);
			auto InstancedComponent = NewObject<UGeneratedModelHISMComponent>(VitruvioModelComponent, FName(UniqueName),
																			  RF_Transient | RF_TextExportTransient | RF_DuplicateTransient);
			const TArray<FTransform>& Transforms = Instance.Transforms;
			InstancedComponent->SetStaticMesh(Instance.InstanceMesh->GetStaticMesh());
			InstancedComponent->SetMeshIdentifier(Instance.InstanceMesh->GetIdentifier());
			
			// Add all instance transforms
			for (const FTransform& Transform : Transforms)
			{
				InstancedComponent->AddInstance(Transform);
			}

			// Apply override materials
			for (int32 MaterialIndex = 0; MaterialIndex < Instance.OverrideMaterials.Num(); ++MaterialIndex)
			{
				InstancedComponent->SetMaterial(MaterialIndex, Instance.OverrideMaterials[MaterialIndex]);
			}

			// Attach and register instance component
			InstancedComponent->AttachToComponent(VitruvioModelComponent, FAttachmentTransformRules::KeepRelativeTransform);
			InstancedComponent->CreationMethod = EComponentCreationMethod::Instance;
			RootComponent->GetOwner()->AddOwnedComponent(InstancedComponent);
			InstancedComponent->OnComponentCreated();
			InstancedComponent->RegisterComponent();
		}

		for (auto& [VitruvioComponent, CallbackProxy] : Item.Tile->GenerateCallbackProxies)
		{
			CallbackProxy->OnAttributesEvaluatedBlueprint.Broadcast();
			CallbackProxy->OnAttributesEvaluated.Broadcast();
			CallbackProxy->OnGenerateCompletedBlueprint.Broadcast();
			CallbackProxy->OnGenerateCompleted.Broadcast();
			CallbackProxy->SetReadyToDestroy();
		}

		Item.Tile->GenerateCallbackProxies.Empty();
		Item.Tile->bIsGenerating = false;
	}
	else
	{
		ProcessGenerateQueueCriticalSection.Unlock();
	}

	if (GenerateAllCallbackProxy)
	{
		TArray<UTile*> Tiles;
		Grid.Tiles.GenerateValueArray(Tiles);
		bool bAllGenerated = Algo::NoneOf(Tiles, [](const UTile* Tile) { return Tile->bIsGenerating; });
		if (bAllGenerated)
		{
			GenerateAllCallbackProxy->OnGenerateCompleted.Broadcast();
			GenerateAllCallbackProxy = nullptr;
		}
	}
}

void AVitruvioBatchActor::ProcessAttributeEvaluationQueue()
{
	ProcessAttributeEvaluationQueueCriticalSection.Lock();
	
	if (!AttributeEvaluationQueue.IsEmpty())
	{
		FEvaluateAttributesQueueItem Item;
		AttributeEvaluationQueue.Dequeue(Item);

		ProcessAttributeEvaluationQueueCriticalSection.Unlock();

		for (int ComponentIndex = 0; ComponentIndex < Item.VitruvioComponents.Num(); ++ComponentIndex)
		{
			UVitruvioComponent* VitruvioComponent = Item.VitruvioComponents[ComponentIndex];
			Item.AttributeMaps[ComponentIndex]->UpdateUnrealAttributeMap(VitruvioComponent->Attributes, VitruvioComponent);
			VitruvioComponent->bAttributesReady = true;
			VitruvioComponent->NotifyAttributesChanged();
		}
	}
	else
	{
		ProcessAttributeEvaluationQueueCriticalSection.Unlock();
	}
}

void AVitruvioBatchActor::Tick(float DeltaSeconds)
{
	ProcessTiles();
	
	ProcessAttributeEvaluationQueue();
	ProcessGenerateQueue();
}

void AVitruvioBatchActor::RegisterVitruvioComponent(UVitruvioComponent* VitruvioComponent, bool bGenerateModel)
{
	if (VitruvioComponents.Contains(VitruvioComponent))
	{
		return;
	}
	
	VitruvioComponents.Add(VitruvioComponent);
	Grid.Register(VitruvioComponent, this, bGenerateModel);
}

void AVitruvioBatchActor::UnregisterVitruvioComponent(UVitruvioComponent* VitruvioComponent)
{
	VitruvioComponents.Remove(VitruvioComponent);
	Grid.Unregister(VitruvioComponent);
}

void AVitruvioBatchActor::UnregisterAllVitruvioComponents()
{
	Grid.Clear();
	VitruvioComponents.Empty();
}

TSet<UVitruvioComponent*> AVitruvioBatchActor::GetVitruvioComponents()
{
	return VitruvioComponents;
}

void AVitruvioBatchActor::EvaluateAttributes(UVitruvioComponent* VitruvioComponent, UGenerateCompletedCallbackProxy* CallbackProxy)
{
	Grid.MarkForAttributeEvaluation(VitruvioComponent, CallbackProxy);
}

void AVitruvioBatchActor::EvaluateAllAttributes(UGenerateCompletedCallbackProxy* CallbackProxy)
{
	EvaluateAllCallbackProxy = CallbackProxy;
	Grid.MarkAllForAttributeEvaluation();
}

void AVitruvioBatchActor::Generate(UVitruvioComponent* VitruvioComponent, UGenerateCompletedCallbackProxy* CallbackProxy)
{
	Grid.MarkForGenerate(VitruvioComponent, CallbackProxy);
}

void AVitruvioBatchActor::GenerateAll(UGenerateCompletedCallbackProxy* CallbackProxy)
{
	GenerateAllCallbackProxy = CallbackProxy;
	Grid.MarkAllForGenerate();
}

bool AVitruvioBatchActor::ShouldTickIfViewportsOnly() const
{
	return true;
}

void AVitruvioBatchActor::SetMaterialReplacementAsset(UMaterialReplacementAsset* MaterialReplacementAsset)
{
	MaterialReplacement = MaterialReplacementAsset;
	GenerateAll();
}

void AVitruvioBatchActor::SetInstanceReplacementAsset(UInstanceReplacementAsset* InstanceReplacementAsset)
{
	InstanceReplacement = InstanceReplacementAsset;
	GenerateAll();
}

#if WITH_EDITOR
bool AVitruvioBatchActor::CanDeleteSelectedActor(FText& OutReason) const
{
	return false;
}

void AVitruvioBatchActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	if (PropertyChangedEvent.MemberProperty &&
		PropertyChangedEvent.MemberProperty->GetFName() == GET_MEMBER_NAME_CHECKED(AVitruvioBatchActor, GridDimension))
	{
		Grid.Clear();
		Grid.RegisterAll(VitruvioComponents, this);
	}

	if (!PropertyChangedEvent.Property)
	{
		return;
	}
	
	if (PropertyChangedEvent.Property->GetFName() == GET_MEMBER_NAME_CHECKED(UVitruvioComponent, MaterialReplacement) ||
		PropertyChangedEvent.Property->GetFName() == GET_MEMBER_NAME_CHECKED(UVitruvioComponent, InstanceReplacement) ||
		PropertyChangedEvent.MemberProperty->GetFName() == GET_MEMBER_NAME_CHECKED(AVitruvioBatchActor, bEnableOcclusionQueries))
	{
		GenerateAll();
	}
}
#endif
