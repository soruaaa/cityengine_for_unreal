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

#pragma once

#include "CoreMinimal.h"

#include "VitruvioModule.h"
#include "GenerateCompletedCallbackProxy.h"
#include "Util/AttributeConversion.h"

#include "VitruvioBatchActor.generated.h"

UCLASS()
class UTile : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(VisibleAnywhere, Category = "Vitruvio")
	TSet<UVitruvioComponent*> VitruvioComponents;
	
	UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = "Vitruvio")
	FIntPoint Location;
	
	UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = "Vitruvio")
	bool bMarkedForGenerate;
	bool bIsGenerating;

	UPROPERTY(BlueprintReadOnly, VisibleAnywhere, Category = "Vitruvio")
	bool bMarkedForEvaluateAttributes;
	bool bIsEvaluatingAttributes;

	UPROPERTY()
	TMap<UVitruvioComponent*, UGenerateCompletedCallbackProxy*> GenerateCallbackProxies;
	UPROPERTY()
	TMap<UVitruvioComponent*, UGenerateCompletedCallbackProxy*> EvaluateAttributesCallbackProxies;

	FBatchGenerateResult::FTokenPtr GenerateToken;
	FAttributeMapsResult::FTokenPtr EvalAttributesToken;

	UPROPERTY()
	UGeneratedModelStaticMeshComponent* GeneratedModelComponent;

	void MarkForAttributeEvaluation(UVitruvioComponent* VitruvioComponent, UGenerateCompletedCallbackProxy* CallbackProxy = nullptr);
	void UnmarkForAttributeEvaluation();
	
	void MarkForGenerate(UVitruvioComponent* VitruvioComponent, UGenerateCompletedCallbackProxy* CallbackProxy = nullptr);
	void UnmarkForGenerate();
	
	void Add(UVitruvioComponent* VitruvioComponent);
	void Remove(UVitruvioComponent* VitruvioComponent);
	bool Contains(UVitruvioComponent* VitruvioComponent) const;

	TTuple<TArray<FInitialShape>, TArray<UVitruvioComponent*>> GetInitialShapes(const TFunction<bool(UVitruvioComponent*)>& Filter);
	TTuple<TArray<FInitialShape>, TArray<UVitruvioComponent*>> GetInitialShapes();
};

USTRUCT()
struct FGrid
{
	GENERATED_BODY()

	UPROPERTY()
	TMap<FIntPoint, UTile*> Tiles;
	UPROPERTY()
	TMap<UVitruvioComponent*, UTile*> TilesByComponent;

	void MarkForAttributeEvaluation(UVitruvioComponent* VitruvioComponent, UGenerateCompletedCallbackProxy* CallbackProxy = nullptr);
	void MarkAllForAttributeEvaluation();

	void MarkForGenerate(UVitruvioComponent* VitruvioComponent, UGenerateCompletedCallbackProxy* CallbackProxy = nullptr);
	void MarkAllForGenerate();
	
	void RegisterAll(const TSet<UVitruvioComponent*>& VitruvioComponents, AVitruvioBatchActor* VitruvioBatchActor, bool bGeneateModel = true);
	void Register(UVitruvioComponent* VitruvioComponent, AVitruvioBatchActor* VitruvioBatchActor, bool bGeneateModel = true);
	void Unregister(UVitruvioComponent* VitruvioComponent);

	void Clear();

	TArray<UTile*> GetTilesMarkedForGenerate() const;
	TArray<UTile*> GetTilesMarkedForAttributeEvaluation() const;
	
	void UnmarkAllForGenerate();
	void UnmarkAllForAttributeEvaluation();

	TArray<FInitialShape> GetNeighboringShapes(const UTile* Tile, const TArray<FInitialShape>& Initial);
};

struct FBatchGenerateQueueItem
{
	FGenerateResultDescription GenerateResultDescription;
	UTile* Tile;
	TArray<UVitruvioComponent*> VitruvioComponents;
};

struct FEvaluateAttributesQueueItem
{
	TArray<FAttributeMapPtr> AttributeMaps;
	UTile* Tile;
	TArray<UVitruvioComponent*> VitruvioComponents;
};

UCLASS(NotBlueprintable, NotPlaceable)
class VITRUVIO_API AVitruvioBatchActor : public AActor
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Vitruvio")
	FIntVector2 GridDimension = {50000, 50000};

	UPROPERTY(EditAnywhere, Category = "Vitruvio")
	bool bEnableOcclusionQueries = false;

#if WITH_EDITORONLY_DATA
	UPROPERTY(EditAnywhere, Category = "Vitruvio")
	bool bDebugVisualizeGrid = false;
#endif
	
private:
	UPROPERTY(Transient)
	FGrid Grid;

	TQueue<FBatchGenerateQueueItem> GenerateQueue;
	TQueue<FEvaluateAttributesQueueItem> AttributeEvaluationQueue;

	UPROPERTY(Transient)
	TMap<UMaterialInterface*, FString> MaterialIdentifiers;
	TMap<FString, int32> UniqueMaterialIdentifiers;

	int NumModelComponents = 0;
	
	UPROPERTY(Transient)
	TSet<UVitruvioComponent*> VitruvioComponents;

	/** Default parent material for opaque geometry. */
	UPROPERTY(EditAnywhere, DisplayName = "Opaque Parent", Category = "Vitruvio Default Materials")
	UMaterial* OpaqueParent;

	/** Default parent material for masked geometry. */
	UPROPERTY(EditAnywhere, DisplayName = "Masked Parent", Category = "Vitruvio Default Materials")
	UMaterial* MaskedParent;

	UPROPERTY(EditAnywhere, DisplayName = "Translucent Parent", Category = "Vitruvio Default Materials")
	UMaterial* TranslucentParent;

	/** The material replacement asset which defines how materials are replaced after generating a model. */
	UPROPERTY(EditAnywhere, Category = "Vitruvio Replacmeents", Setter = SetMaterialReplacementAsset)
	UMaterialReplacementAsset* MaterialReplacement;

	/** The instance replacement asset which defines how instances are replaced after generating a model. */
	UPROPERTY(EditAnywhere, Category = "Vitruvio Replacmeents", Setter = SetInstanceReplacementAsset)
	UInstanceReplacementAsset* InstanceReplacement;

public:
	AVitruvioBatchActor();

	virtual void Tick(float DeltaSeconds) override;

	void RegisterVitruvioComponent(UVitruvioComponent* VitruvioComponent, bool bGenerateModel = true);
	void UnregisterVitruvioComponent(UVitruvioComponent* VitruvioComponent);
	void UnregisterAllVitruvioComponents();
	TSet<UVitruvioComponent*> GetVitruvioComponents();

	void EvaluateAttributes(UVitruvioComponent* VitruvioComponent, UGenerateCompletedCallbackProxy* CallbackProxy = nullptr);
	void EvaluateAllAttributes(UGenerateCompletedCallbackProxy* CallbackProxy = nullptr);
	
	void Generate(UVitruvioComponent* VitruvioComponent, UGenerateCompletedCallbackProxy* CallbackProxy = nullptr);
	void GenerateAll(UGenerateCompletedCallbackProxy* CallbackProxy = nullptr);
	
	FIntPoint GetPosition(const UVitruvioComponent* VitruvioComponent) const;
	
#if WITH_EDITOR
	virtual bool CanDeleteSelectedActor(FText& OutReason) const override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	virtual bool ShouldTickIfViewportsOnly() const override;

	/**
	 * Sets the material replacement Asset and regenerates the model.
	 */
	UFUNCTION(BlueprintCallable, Category = "Vitruvio Replacmeents")
	void SetMaterialReplacementAsset(UMaterialReplacementAsset* MaterialReplacementAsset);

	/**
	 * Sets the instance replacement Asset and regenerates the model.
	 */
	UFUNCTION(BlueprintCallable, Category = "Vitruvio Replacmeents")
	void SetInstanceReplacementAsset(UInstanceReplacementAsset* InstanceReplacementAsset);

	
private:
	void ProcessTiles();
	void ProcessGenerateQueue();
	void ProcessAttributeEvaluationQueue();

	FCriticalSection ProcessGenerateQueueCriticalSection;
	FCriticalSection ProcessAttributeEvaluationQueueCriticalSection;

	UPROPERTY()
	UGenerateCompletedCallbackProxy* GenerateAllCallbackProxy;
	UPROPERTY()
	UGenerateCompletedCallbackProxy* EvaluateAllCallbackProxy;
};
