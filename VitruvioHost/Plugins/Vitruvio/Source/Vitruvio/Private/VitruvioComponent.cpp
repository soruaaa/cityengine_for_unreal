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

#include "VitruvioComponent.h"

#include "Util/AttributeConversion.h"
#include "EngineUtils.h"
#include "GenerateCompletedCallbackProxy.h"
#include "GeneratedModelHISMComponent.h"
#include "GeneratedModelStaticMeshComponent.h"
#include "UnrealCallbacks.h"
#include "VitruvioModule.h"
#include "VitruvioTypes.h"

#include "Algo/Transform.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/SplineComponent.h"
#include "Engine/CollisionProfile.h"
#include "PRTUtils.h"
#include "VitruvioBatchSubsystem.h"
#include "UObject/ConstructorHelpers.h"
#include "Engine/World.h"
#include "Materials/Material.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY(LogVitruvioComponent);

TAutoConsoleVariable<float> CVarInterOcclusionNeighborQueryDistance(TEXT("Esri.Vitruvio.InterOcclusionNeighborQueryDistance"), 10000.0f, TEXT("The distance in cm to query for inter-occlusion neighbors."));

namespace
{

int64 InitialShapeCounter;

bool ToBool(const FString& Value)
{
	if (Value.ToLower() == "true")
	{
		return true;
	}
	if (Value.IsNumeric())
	{
		const int32 NumericValue = StaticCast<int32>(FCString::Atof(*Value));
		return NumericValue != 0;
	}
	return false;
}

template <typename T>
void SetInitialShape(UVitruvioComponent* Component, bool bEvaluateAttributes, bool bGenerateModel, UGenerateCompletedCallbackProxy* CallbackProxy, T&& InitialShapeInitializer)
{
	if (Component->InitialShape)
	{
		Component->DestroyInitialShapeComponent();
		Component->InitialShape->Rename(nullptr, GetTransientPackage()); // Remove from Owner
	}

	UInitialShape* NewInitialShape = InitialShapeInitializer();

	Component->InitialShape = NewInitialShape;

	NewInitialShape->UpdatePolygon(Component);

	Component->RemoveGeneratedMeshes();

	if (bEvaluateAttributes || bGenerateModel)
	{
		Component->EvaluateRuleAttributes(bGenerateModel, CallbackProxy);
	}
}

FVector GetCentroid(const TArray<FVector>& Vertices)
{
	FVector Centroid = FVector::ZeroVector;
	for (const FVector& Vertex : Vertices)
	{
		Centroid += Vertex;
	}
	Centroid /= FMath::Max(1, Vertices.Num());

	return Centroid;
}

template <typename A, typename T>
bool GetAttribute(const TMap<FString, URuleAttribute*>& Attributes, const FString& Name, T& OutValue)
{
	URuleAttribute const* const* FoundAttribute = Attributes.Find(Name);
	if (!FoundAttribute)
	{
		OutValue = {};
		return false;
	}

	URuleAttribute const* Attribute = *FoundAttribute;
	A const* TAttribute = Cast<A>(Attribute);
	if (!TAttribute)
	{
		OutValue = {};
		return false;
	}

	if constexpr (TIsTArray<T>::Value)
	{
		OutValue = TAttribute->Values;
	}
	else
	{
		OutValue = TAttribute->Value;
	}

	return true;
}

template <typename A, typename T>
void SetAttribute(UVitruvioComponent* VitruvioComponent, const TMap<FString, URuleAttribute*>& Attributes, const FString& Name, const T& Value,
				  bool bEvaluateAttributes, bool bGenerateModel, UGenerateCompletedCallbackProxy* CallbackProxy)
{
	URuleAttribute* const* FoundAttribute = Attributes.Find(Name);
	if (!FoundAttribute)
	{
		return;
	}

	URuleAttribute* Attribute = FoundAttribute ? *FoundAttribute : nullptr;
	A* TAttribute = Cast<A>(Attribute);
	if (!TAttribute)
	{
		return;
	}

	if constexpr (TIsTArray<T>::Value)
	{
		TAttribute->Values = Value;
	}
	else
	{
		TAttribute->Value = Value;
	}

	TAttribute->bUserSet = true;

	if (bEvaluateAttributes)
	{
		VitruvioComponent->EvaluateRuleAttributes(bGenerateModel, CallbackProxy);
	}
}

template <typename TAttributeType, typename TValueType>
requires std::is_base_of_v<URuleAttribute, TAttributeType>
void SetAttribute(UVitruvioComponent* VitruvioComponent, TMap<FString, URuleAttribute*>& Attributes, const FString& Name, const TValueType& Value,
							 bool bEvaluateAttributes, bool bGenerateModel, UGenerateCompletedCallbackProxy* CallbackProxy)
{
	VitruvioComponent->Initialize();

	URuleAttribute* Attribute;
		
	if (URuleAttribute** AttributeResult = Attributes.Find(Name))
	{
		Attribute = *AttributeResult;
	}
	else
	{
		Attribute = NewObject<TAttributeType>(VitruvioComponent);
		Attribute->SetFlags(RF_Transactional);
		Attribute->Name = Name;
		Attribute->DisplayName = WCHAR_TO_TCHAR(prtu::removeImport(prtu::removeStyle(*Name)).c_str());

		Attributes.Add(Name, Attribute);
	}

	TAttributeType* TAttribute = Cast<TAttributeType>(Attribute);
	if (!TAttribute)
	{
		return;
	}

	if constexpr (TIsTArray<TValueType>::Value)
	{
		TAttribute->Values = Value;
	}
	else
	{
		TAttribute->Value = Value;
	}

	TAttribute->bUserSet = true;

	if (bEvaluateAttributes || bGenerateModel)
	{
		VitruvioComponent->EvaluateRuleAttributes(bGenerateModel, CallbackProxy);
	}
}

bool IsOuterOf(UObject* Inner, UObject* Outer)
{
	if (!Outer)
	{
		return false;
	}

	UObject* Current = Inner->GetOuter();

	while (Current != nullptr)
	{
		if (Current == Outer)
		{
			return true;
		}

		Current = Current->GetOuter();
	}

	return false;
}

#if WITH_EDITOR
bool IsRelevantObject(UVitruvioComponent* VitruvioComponent, UObject* Object)
{
	if (VitruvioComponent == Object || VitruvioComponent->InitialShape == Object)
	{
		return true;
	}
	AActor* Owner = VitruvioComponent->GetOwner();
	if (!Owner)
	{
		return false;
	}

	const TSet<UActorComponent*>& Components = Owner->GetComponents();
	for (UActorComponent* ChildComponent : Components)
	{
		if (ChildComponent == Object)
		{
			return true;
		}
	}

	return false;
}
#endif

} // namespace

UVitruvioComponent::FOnHierarchyChanged UVitruvioComponent::OnHierarchyChanged;
UVitruvioComponent::FOnAttributesChanged UVitruvioComponent::OnAttributesChanged;

void ApplyMaterialReplacements(UStaticMeshComponent* StaticMeshComponent, const TMap<UMaterialInterface*, FString>& MaterialIdentifiers,
							   UMaterialReplacementAsset* Replacement)
{
	if (!Replacement)
	{
		return;
	}

	TMap<FString, UMaterialInterface*> ReplacementMaterials;
	for (const FMaterialReplacementData& ReplacementData : Replacement->Replacements)
	{
		ReplacementMaterials.Add(ReplacementData.MaterialIdentifier, ReplacementData.ReplacementMaterial);
	}

	for (int32 MaterialIndex = 0; MaterialIndex < StaticMeshComponent->GetNumMaterials(); ++MaterialIndex)
	{
		const UMaterialInterface* SourceMaterial = StaticMeshComponent->GetMaterial(MaterialIndex);
		FString MaterialIdentifier = MaterialIdentifiers[SourceMaterial];

		if (UMaterialInterface** Result = ReplacementMaterials.Find(MaterialIdentifier))
		{
			UMaterialInterface* ReplacementMaterial = *Result;
			StaticMeshComponent->SetMaterial(MaterialIndex, ReplacementMaterial);
		}
	}
}

TSet<FInstance> ApplyInstanceReplacements(UGeneratedModelStaticMeshComponent* GeneratedModelComponent, 
											  const TArray<FInstance>& Instances, UInstanceReplacementAsset* Replacement, TMap<FString, int32>& NameMap)
{
	TSet<FInstance> Replaced;
	if (!Replacement)
	{
		return Replaced;
	}

	TMap<FString, FInstanceReplacement> InstanceReplacementMap;

	for (const FInstanceReplacement& ReplacementData : Replacement->Replacements)
	{
		InstanceReplacementMap.Add(ReplacementData.SourceMeshIdentifier, ReplacementData);
	}

	for (const FInstance& Instance : Instances)
	{
		if (FInstanceReplacement* InstanceReplacement = InstanceReplacementMap.Find(Instance.InstanceMesh->GetIdentifier()))
		{
			if (!InstanceReplacement->HasReplacement())
			{
				continue;
			}

			TArray<UGeneratedModelHISMComponent*> InstancedComponents;

			TArray<float> CumulativeProbabilities;
			float CumulativeProbability = 0.0f;
			for (const FReplacementOption& ReplacementOption : InstanceReplacement->Replacements)
			{
				CumulativeProbability += ReplacementOption.Frequency;
				CumulativeProbabilities.Add(CumulativeProbability);

				FString UniqueName = UniqueComponentName(ReplacementOption.Mesh->GetName(), NameMap);
				auto InstancedComponent = NewObject<UGeneratedModelHISMComponent>(GeneratedModelComponent, FName(UniqueName),
																				  RF_Transient | RF_TextExportTransient | RF_DuplicateTransient);
				InstancedComponent->SetStaticMesh(ReplacementOption.Mesh.Get());
				InstancedComponents.Add(InstancedComponent);
			}

			for (const auto& InstancedComponent : InstancedComponents)
			{
				// Attach and register instance component
				InstancedComponent->AttachToComponent(GeneratedModelComponent,
													  FAttachmentTransformRules::KeepRelativeTransform);
				InstancedComponent->CreationMethod = EComponentCreationMethod::Instance;
				GeneratedModelComponent->GetOwner()->AddOwnedComponent(InstancedComponent);
				InstancedComponent->OnComponentCreated();
				InstancedComponent->RegisterComponent();
			}

			auto RandomVector = [](const FVector& Min, const FVector& Max) {
				double RandX = FMath::RandRange(Min[0], Max[0]);
				double RandY = FMath::RandRange(Min[1], Max[1]);
				double RandZ = FMath::RandRange(Min[2], Max[2]);
				return FVector{RandX, RandY, RandZ};
			};

			TMap<int, TArray<FTransform>>  ModifiedTransforms;
			for (const FTransform& Transform : Instance.Transforms)
			{
				const float RandomProbability = FMath::RandRange(0.0f, CumulativeProbability);
				const int ComponentIndex = Algo::LowerBound(CumulativeProbabilities, RandomProbability);

				const FReplacementOption& ReplacementOption = InstanceReplacement->Replacements[ComponentIndex];
				FTransform ModifiedTransform = Transform;
				if (ReplacementOption.bRandomScale)
				{
					if (ReplacementOption.bUniformScale)
					{
						const double RandScale = FMath::RandRange(ReplacementOption.UniformMinScale, ReplacementOption.UniformMaxScale);
						ModifiedTransform.SetScale3D({RandScale, RandScale, RandScale});
					}
					else
					{
						ModifiedTransform.SetScale3D(RandomVector(ReplacementOption.MinScale, ReplacementOption.MaxScale));
					}
				}

				if (ReplacementOption.bRandomRotation)
				{
					ModifiedTransform.SetRotation(FQuat::MakeFromEuler(RandomVector(ReplacementOption.MinRotation, ReplacementOption.MaxRotation)));
				}

				ModifiedTransforms.FindOrAdd(ComponentIndex).Add(ModifiedTransform);
				
			}

			for (const auto& [ComponentIndex, Transforms] : ModifiedTransforms)
			{
				InstancedComponents[ComponentIndex]->AddInstances(Transforms, false);
			}

			Replaced.Add(Instance);
		}
	}
	return Replaced;
}

FConvertedGenerateResult BuildGenerateResult(const FGenerateResultDescription& GenerateResult,
									 TMap<Vitruvio::FMaterialAttributeContainer, TObjectPtr<UMaterialInstanceDynamic>>& MaterialCache,
									 TMap<FString, Vitruvio::FTextureData>& TextureCache,
									 TMap<UMaterialInterface*, FString>& MaterialIdentifiers,
									 TMap<FString, int32>& UniqueMaterialIdentifiers,
									 UMaterial* OpaqueParent, UMaterial* MaskedParent, UMaterial* TranslucentParent,
									 UWorld* World)
{
	MaterialIdentifiers.Empty();
	UniqueMaterialIdentifiers.Empty();

	// Build all meshes
	if (GenerateResult.GeneratedModel)
	{
		GenerateResult.GeneratedModel->Build(TEXT("GeneratedModel"), MaterialCache, TextureCache, MaterialIdentifiers, UniqueMaterialIdentifiers,
			OpaqueParent, MaskedParent, TranslucentParent, World);
	}

	for (const auto& IdAndMesh : GenerateResult.InstanceMeshes)
	{
		FString Name = GenerateResult.InstanceNames[IdAndMesh.Key];
		IdAndMesh.Value->Build(Name, MaterialCache, TextureCache, MaterialIdentifiers, UniqueMaterialIdentifiers,
			OpaqueParent, MaskedParent, TranslucentParent, World);
	}

	// Convert instances
	TArray<FInstance> Instances;
	for (const auto& [Key, Transform] : GenerateResult.Instances)
	{
		const TSharedPtr<FVitruvioMesh>& VitruvioMesh = GenerateResult.InstanceMeshes[Key.MeshId];
		const FString MeshName = GenerateResult.InstanceNames[Key.MeshId];
		TArray<UMaterialInstanceDynamic*> OverrideMaterials;

		for (size_t MaterialIndex = 0; MaterialIndex < Key.MaterialOverrides.Num(); ++MaterialIndex)
		{
			const Vitruvio::FMaterialAttributeContainer& MaterialContainer = Key.MaterialOverrides[MaterialIndex];
			OverrideMaterials.Add(CacheMaterial(OpaqueParent, MaskedParent, TranslucentParent, TextureCache, MaterialCache, MaterialContainer,
												UniqueMaterialIdentifiers, MaterialIdentifiers, VitruvioMesh->GetStaticMesh()));
		}

		Instances.Add({MeshName, VitruvioMesh, OverrideMaterials, Transform});
	}

	return {GenerateResult.GeneratedModel, Instances, GenerateResult.Reports};
}

FString UniqueComponentName(const FString& Name, TMap<FString, int32>& UsedNames)
{
	FString CurrentName = Name;
	while (UsedNames.Contains(CurrentName))
	{
		const int32 Count = UsedNames[CurrentName]++;
		CurrentName = Name + FString::FromInt(Count);
	}
	UsedNames.Add(CurrentName, 0);
	return CurrentName;
}

UVitruvioComponent::UVitruvioComponent()
{
	static ConstructorHelpers::FObjectFinder<UMaterial> Opaque(TEXT("Material'/Vitruvio/Materials/M_OpaqueParent.M_OpaqueParent'"));
	static ConstructorHelpers::FObjectFinder<UMaterial> Masked(TEXT("Material'/Vitruvio/Materials/M_MaskedParent.M_MaskedParent'"));
	static ConstructorHelpers::FObjectFinder<UMaterial> Translucent(TEXT("Material'/Vitruvio/Materials/M_TranslucentParent.M_TranslucentParent'"));
	OpaqueParent = Opaque.Object;
	MaskedParent = Masked.Object;
	TranslucentParent = Translucent.Object;

	PrimaryComponentTick.bCanEverTick = true;
	bTickInEditor = true;

	InitialShapeIndex = InitialShapeCounter++;
}

int64 UVitruvioComponent::GetInitialShapeIndex() const
{
	return InitialShapeIndex;
}

FInitialShape UVitruvioComponent::GetInitialShape() const
{
	TMap<FString, TWeakObjectPtr<URuleAttribute>> WeakAttributes;

	for (const auto& Pair : Attributes)
	{
		WeakAttributes.Add(Pair.Key, TWeakObjectPtr(Pair.Value));
	}
	
	return FInitialShape { InitialShapeIndex, GetOwner()->GetTransform().GetLocation(), InitialShape->GetPolygon(), WeakAttributes, RandomSeed, Rpk };
}

TArray<FInitialShape> UVitruvioComponent::GetNeighboringShapes() const
{
	TArray<FInitialShape> NeighboringShapes;

	const FVector OwnerLocation = GetOwner()->GetActorLocation();

	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		AActor* Actor = *It;

		if (Actor == GetOwner())
		{
			continue;
		}

		UVitruvioComponent* VitruvioComponent = Actor->FindComponentByClass<UVitruvioComponent>();
		if (!VitruvioComponent || !VitruvioComponent->bEnableOcclusionQueries  || !VitruvioComponent->InitialShape || !VitruvioComponent->InitialShape->IsValid())
		{
			continue;
		}

		const float Distance = FVector::Dist(OwnerLocation, Actor->GetActorLocation());
		if (Distance < CVarInterOcclusionNeighborQueryDistance.GetValueOnGameThread())
		{
			NeighboringShapes.Add(VitruvioComponent->GetInitialShape());
		}
	}

	return NeighboringShapes;
}

void UVitruvioComponent::CalculateRandomSeed()
{
	if (!bValidRandomSeed && InitialShape && InitialShape->IsValid())
	{
		const FVector Centroid = GetCentroid(InitialShape->GetVertices());
		RandomSeed = GetTypeHash(GetOwner()->GetActorTransform().TransformPosition(Centroid));
		bValidRandomSeed = true;
	}
}

bool UVitruvioComponent::HasValidInputData() const
{
	return InitialShape && InitialShape->IsValid() && Rpk;
}

bool UVitruvioComponent::IsReadyToGenerate() const
{
	return HasValidInputData() && bAttributesReady;
}

void UVitruvioComponent::SetRpk(URulePackage* RulePackage, bool bEvaluateAttributes, bool bGenerateModel, UGenerateCompletedCallbackProxy* CallbackProxy)
{
	if (Rpk == RulePackage)
	{
		return;
	}

	Rpk = RulePackage;

	Attributes.Empty();
	bAttributesReady = false;
	bNotifyAttributeChange = true;

	RemoveGeneratedMeshes();

	if (bEvaluateAttributes)
	{
		EvaluateRuleAttributes(bGenerateModel, CallbackProxy);
	}
}

void UVitruvioComponent::SetBatchGenerated(bool bBatchGeneration, bool bGenerateModel)
{
	if (GenerateToken)
	{
		GenerateToken->Invalidate();
		GenerateToken.Reset();
	}
	
	bBatchGenerate = bBatchGeneration;
	
	UVitruvioBatchSubsystem* VitruvioSubsystem = GetWorld()->GetSubsystem<UVitruvioBatchSubsystem>();
	if (bBatchGenerate)
	{
		RemoveGeneratedMeshes();
		VitruvioSubsystem->RegisterVitruvioComponent(this, bGenerateModel);
	}
	else
	{
		VitruvioSubsystem->UnregisterVitruvioComponent(this);

		if (bGenerateModel)
		{
			EvaluateRuleAttributes(true, nullptr);
		}
	}
}

bool UVitruvioComponent::IsBatchGenerated() const
{
	return bBatchGenerate;
}

void UVitruvioComponent::SetMaterialReplacementAsset(UMaterialReplacementAsset* MaterialReplacementAsset)
{
	MaterialReplacement = MaterialReplacementAsset;
	Generate();
}

void UVitruvioComponent::SetInstanceReplacementAsset(UInstanceReplacementAsset* InstanceReplacementAsset)
{
	InstanceReplacement = InstanceReplacementAsset;
	Generate();
}

void UVitruvioComponent::SetStringAttribute(const FString& Name, const FString& Value, bool bEvaluateAttributes, bool bGenerateModel,
											UGenerateCompletedCallbackProxy* CallbackProxy)
{
	SetAttribute<UStringAttribute, FString>(this, Attributes, Name, Value, bEvaluateAttributes, bGenerateModel, CallbackProxy);
}

bool UVitruvioComponent::GetStringAttribute(const FString& Name, FString& OutValue) const
{
	return GetAttribute<UStringAttribute, FString>(Attributes, Name, OutValue);
}

void UVitruvioComponent::SetStringArrayAttribute(const FString& Name, const TArray<FString>& Values, bool bEvaluateAttributes, bool bGenerateModel,
												 UGenerateCompletedCallbackProxy* CallbackProxy)
{
	SetAttribute<UStringArrayAttribute, TArray<FString>>(this, Attributes, Name, Values, bEvaluateAttributes, bGenerateModel, CallbackProxy);
}

bool UVitruvioComponent::GetStringArrayAttribute(const FString& Name, TArray<FString>& OutValue) const
{
	return GetAttribute<UStringArrayAttribute, TArray<FString>>(Attributes, Name, OutValue);
}

void UVitruvioComponent::SetBoolAttribute(const FString& Name, bool Value, bool bEvaluateAttributes, bool bGenerateModel, UGenerateCompletedCallbackProxy* CallbackProxy)
{
	SetAttribute<UBoolAttribute, bool>(this, Attributes, Name, Value, bEvaluateAttributes, bGenerateModel, CallbackProxy);
}

bool UVitruvioComponent::GetBoolAttribute(const FString& Name, bool& OutValue) const
{
	return GetAttribute<UBoolAttribute, bool>(Attributes, Name, OutValue);
}

void UVitruvioComponent::SetBoolArrayAttribute(const FString& Name, const TArray<bool>& Values, bool bEvaluateAttributes, bool bGenerateModel,
											   UGenerateCompletedCallbackProxy* CallbackProxy)
{
	SetAttribute<UBoolArrayAttribute, TArray<bool>>(this, Attributes, Name, Values, bEvaluateAttributes, bGenerateModel, CallbackProxy);
}

bool UVitruvioComponent::GetBoolArrayAttribute(const FString& Name, TArray<bool>& OutValue) const
{
	return GetAttribute<UBoolArrayAttribute, TArray<bool>>(Attributes, Name, OutValue);
}

void UVitruvioComponent::SetFloatAttribute(const FString& Name, double Value, bool bEvaluateAttributes, bool bGenerateModel, UGenerateCompletedCallbackProxy* CallbackProxy)
{
	SetAttribute<UFloatAttribute, double>(this, Attributes, Name, Value, bEvaluateAttributes, bGenerateModel, CallbackProxy);
}

bool UVitruvioComponent::GetFloatAttribute(const FString& Name, double& OutValue) const
{
	return GetAttribute<UFloatAttribute, double>(Attributes, Name, OutValue);
}

void UVitruvioComponent::SetFloatArrayAttribute(const FString& Name, const TArray<double>& Values, bool bEvaluateAttributes, bool bGenerateModel,
												UGenerateCompletedCallbackProxy* CallbackProxy)
{
	SetAttribute<UFloatArrayAttribute, TArray<double>>(this, Attributes, Name, Values, bEvaluateAttributes, bGenerateModel, CallbackProxy);
}

bool UVitruvioComponent::GetFloatArrayAttribute(const FString& Name, TArray<double>& OutValue) const
{
	return GetAttribute<UFloatArrayAttribute, TArray<double>>(Attributes, Name, OutValue);
}

void UVitruvioComponent::SetAttributes(const TMap<FString, FString>& NewAttributes, bool bEvaluateAttributes, bool bGenerateModel, UGenerateCompletedCallbackProxy* CallbackProxy)
{
	Initialize();
	for (const auto& KeyValues : NewAttributes)
	{
		const FString& Value = KeyValues.Value;
		const FString& Key = KeyValues.Key;

		URuleAttribute* Attribute;
		
		if (URuleAttribute* const* AttributeResult = GetAttributes().Find(Key))
		{
			Attribute = *AttributeResult;
		}
		else
		{
			Attribute = Vitruvio::CreateAttribute(Key, Value);
			Attributes.Add(Key, Attribute);
		}

		if (Cast<UFloatAttribute>(Attribute))
		{
			SetAttribute<UFloatAttribute, double>(this, GetAttributes(), Key, FCString::Atof(*Value), false,
			                                      false, nullptr);
		}
		else if (Cast<UBoolAttribute>(Attribute))
		{
			SetAttribute<UBoolAttribute, bool>(this, GetAttributes(), Key, ToBool(Value), false, false, nullptr);
		}
		else if (Cast<UStringAttribute>(Attribute))
		{
			SetAttribute<UStringAttribute, FString>(this, GetAttributes(), Key, Value, false, false, nullptr);
		}
		else if (Cast<UArrayAttribute>(Attribute))
		{
			FString ArrayValue = Value;
			if (Value.StartsWith(TEXT("[")) && Value.EndsWith(TEXT("]")))
			{
				ArrayValue = Value.LeftChop(1).RightChop(1);
			}

			TArray<FString> StringValues;
			ArrayValue.ParseIntoArray(StringValues, TEXT(","));

			if (Cast<UFloatArrayAttribute>(Attribute))
			{
				TArray<double> DoubleValues;
				Algo::Transform(StringValues, DoubleValues, [](const auto& In) { return FCString::Atof(*In); });
				SetAttribute<UFloatArrayAttribute, TArray<double>>(this, GetAttributes(), Key, DoubleValues,
				                                                   false, false, nullptr);
			}
			else if (Cast<UBoolArrayAttribute>(Attribute))
			{
				TArray<bool> BoolValues;
				Algo::Transform(StringValues, BoolValues, [](const auto& In) { return ToBool(In); });
				SetAttribute<UBoolArrayAttribute, TArray<bool>>(this, GetAttributes(), Key, BoolValues, false,
				                                                false, nullptr);
			}
			else
			{
				SetAttribute<UStringArrayAttribute, TArray<FString>>(this, GetAttributes(), Key, StringValues,
				                                                     false, false, nullptr);
			}
		}
	}

	if (bEvaluateAttributes || bGenerateModel)
	{
		EvaluateRuleAttributes(bGenerateModel, CallbackProxy);
	}
}

void UVitruvioComponent::SetMeshInitialShape(UStaticMesh* StaticMesh, bool bEvaluateAttributes, bool bGenerateModel, UGenerateCompletedCallbackProxy* CallbackProxy)
{
	SetInitialShape(this, bEvaluateAttributes, bGenerateModel, CallbackProxy, [this, StaticMesh]() {
		UStaticMeshInitialShape* NewInitialShape =
			NewObject<UStaticMeshInitialShape>(GetOwner(), UStaticMeshInitialShape::StaticClass(), NAME_None, RF_Transactional);
		InitialShapeSceneComponent = NewInitialShape->CreateInitialShapeComponent(this, StaticMesh);
		return NewInitialShape;
	});
}

void UVitruvioComponent::SetSplineInitialShape(const TArray<FSplinePoint>& SplinePoints, bool bEvaluateAttributes, bool bGenerateModel,
											   UGenerateCompletedCallbackProxy* CallbackProxy)
{
	SetInitialShape(this, bEvaluateAttributes, bGenerateModel, CallbackProxy, [this, &SplinePoints]() {
		USplineInitialShape* NewInitialShape =
			NewObject<USplineInitialShape>(GetOwner(), USplineInitialShape::StaticClass(), NAME_None, RF_Transactional);
		InitialShapeSceneComponent = NewInitialShape->CreateInitialShapeComponent(this, SplinePoints);
		return NewInitialShape;
	});
}

void UVitruvioComponent::SetPolygonInitialShape(const FInitialShapePolygon& InitialShapePolygon, bool bEvaluateAttributes, bool bGenerateModel,
	UGenerateCompletedCallbackProxy* CallbackProxy)
{
	SetInitialShape(this, bEvaluateAttributes, bGenerateModel, CallbackProxy, [this, InitialShapePolygon]() {
		UPolygonInitialShape* NewInitialShape =
			NewObject<UPolygonInitialShape>(GetOwner(), UPolygonInitialShape::StaticClass(), NAME_None, RF_Transactional);
		InitialShapeSceneComponent = NewInitialShape->CreateInitialShapeComponent(this, InitialShapePolygon);
		return NewInitialShape;
	});
}

const TMap<FString, URuleAttribute*>& UVitruvioComponent::GetAttributes() const
{
	return Attributes;
}

URulePackage* UVitruvioComponent::GetRpk() const
{
	return Rpk;
}

const TMap<FString, FReport>& UVitruvioComponent::GetReports() const
{
	return Reports;
}

void UVitruvioComponent::SetInitialShapeVisible(bool bVisible)
{
	InitialShapeSceneComponent->SetVisibility(bVisible, false);
	InitialShapeSceneComponent->SetHiddenInGame(!bVisible);
}

void UVitruvioComponent::SetRandomSeed(int32 NewRandomSeed, bool bEvaluateAttributes, bool bGenerateModel, UGenerateCompletedCallbackProxy* CallbackProxy)
{
	RandomSeed = NewRandomSeed;
	bValidRandomSeed = true;

	if (bEvaluateAttributes || bGenerateModel)
	{
		EvaluateRuleAttributes(bGenerateModel, CallbackProxy);
	}
}

int32 UVitruvioComponent::GetRandomSeed()
{
	return RandomSeed;
}

void UVitruvioComponent::LoadInitialShape()
{
	if (InitialShape)
	{
		InitialShapeSceneComponent = InitialShape->CreateInitialShapeComponent(this);
		return;
	}

	// Detect initial initial-shape type (e.g. if there has already been a StaticMeshComponent assigned to the actor)
	check(GetInitialShapesClasses().Num() > 0);
	for (const auto& InitialShapeClasses : GetInitialShapesClasses())
	{
		UInitialShape* DefaultInitialShape = Cast<UInitialShape>(InitialShapeClasses->GetDefaultObject());
		if (DefaultInitialShape && DefaultInitialShape->CanConstructFrom(this->GetOwner()))
		{
			InitialShape = NewObject<UInitialShape>(GetOwner(), DefaultInitialShape->GetClass(), NAME_None, RF_Transactional);
			break;
		}
	}

	if (!InitialShape)
	{
		InitialShape = NewObject<UInitialShape>(GetOwner(), GetInitialShapesClasses()[0], NAME_None, RF_Transactional);
	}

	InitialShape->Initialize();
	InitialShapeSceneComponent = InitialShape->CreateInitialShapeComponent(this);
	InitialShape->UpdatePolygon(this);
}

void UVitruvioComponent::Initialize()
{
	if (bInitialized)
	{
		return;
	}

	bInitialized = true;

	// During cooking we don't need to do initialize anything as we don't wont to generate during the cooking process
	if (GIsCookerLoadingPackage)
	{
		return;
	}

#if WITH_EDITOR
	if (!PropertyChangeDelegate.IsValid())
	{
		PropertyChangeDelegate = FCoreUObjectDelegates::OnObjectPropertyChanged.AddUObject(this, &UVitruvioComponent::OnPropertyChanged);
	}
	
	auto ActorMoved = [this](const AActor* Actor)
	{
		if (Actor != GetOwner())
		{
			return;
		}
		
		if (UVitruvioComponent* VitruvioComponent = Actor->FindComponentByClass<UVitruvioComponent>())
		{
			if (!VitruvioComponent->IsBatchGenerated() && VitruvioComponent->bEnableOcclusionQueries && VitruvioComponent->GenerateAutomatically)
			{
				VitruvioComponent->Generate();
			}
		}
	};
	
	OnActorMoved = GEngine->OnActorMoved().AddLambda([ActorMoved](AActor* Actor)
	{
		ActorMoved(Actor);
	});

	OnActorsMoved = GEngine->OnActorsMoved().AddLambda([ActorMoved](const TArray<AActor*>& Actors)
	{
		for (const AActor* Actor : Actors)
		{
			ActorMoved(Actor);
		}
	});

#endif

	LoadInitialShape();

	OnHierarchyChanged.Broadcast(this);

	CalculateRandomSeed();

	if (bBatchGenerate)
	{
		UVitruvioBatchSubsystem* BatchGenerateSubsystem = GetWorld()->GetSubsystem<UVitruvioBatchSubsystem>();
		BatchGenerateSubsystem->RegisterVitruvioComponent(this);
	}
	else
	{
		EvaluateRuleAttributes(GenerateAutomatically);
	}
}

void UVitruvioComponent::ProcessGenerateQueue()
{
	if (GenerateQueue.IsEmpty())
	{
		return;
	}

	if (bBatchGenerate)
	{
		RemoveGeneratedMeshes();
		GenerateQueue.Empty();
		return;
	}
		
	// Get from queue and build meshes
	FGenerateQueueItem Result;
	GenerateQueue.Dequeue(Result);

	FConvertedGenerateResult ConvertedResult = BuildGenerateResult(Result.GenerateResultDescription,
VitruvioModule::Get().GetMaterialCache(), VitruvioModule::Get().GetTextureCache(),
			MaterialIdentifiers, UniqueMaterialIdentifiers, OpaqueParent, MaskedParent, TranslucentParent, GetWorld());

	Reports = ConvertedResult.Reports;

	QUICK_SCOPE_CYCLE_COUNTER(STAT_VitruvioActor_CreateModelActors);

	UGeneratedModelStaticMeshComponent* VitruvioModelComponent = nullptr;

	TArray<USceneComponent*> InitialShapeChildComponents;
	InitialShapeSceneComponent->GetChildrenComponents(false, InitialShapeChildComponents);
	for (USceneComponent* Component : InitialShapeChildComponents)
	{
		if (Component->IsA(UGeneratedModelStaticMeshComponent::StaticClass()))
		{
			VitruvioModelComponent = Cast<UGeneratedModelStaticMeshComponent>(Component);

			VitruvioModelComponent->SetStaticMesh(nullptr);

			// Cleanup old hierarchical instances
			TArray<USceneComponent*> InstanceComponents;
			VitruvioModelComponent->GetChildrenComponents(true, InstanceComponents);
			for (USceneComponent* InstanceComponent : InstanceComponents)
			{
				InstanceComponent->DestroyComponent(true);
			}

			break;
		}
	}

	if (!VitruvioModelComponent)
	{
		VitruvioModelComponent = NewObject<UGeneratedModelStaticMeshComponent>(InitialShapeSceneComponent, FName(TEXT("GeneratedModel")),
																			   RF_Transient | RF_TextExportTransient | RF_DuplicateTransient);
		VitruvioModelComponent->CreationMethod = EComponentCreationMethod::Instance;
		InitialShapeSceneComponent->GetOwner()->AddOwnedComponent(VitruvioModelComponent);
		VitruvioModelComponent->AttachToComponent(InitialShapeSceneComponent, FAttachmentTransformRules::KeepRelativeTransform);
		VitruvioModelComponent->OnComponentCreated();
		VitruvioModelComponent->RegisterComponent();
	}

	if (ConvertedResult.ShapeMesh)
	{
		VitruvioModelComponent->SetStaticMesh(ConvertedResult.ShapeMesh->GetStaticMesh());
		VitruvioModelComponent->RecreatePhysicsState();
		
		// Reset Material replacements
		for (int32 MaterialIndex = 0; MaterialIndex < VitruvioModelComponent->GetNumMaterials(); ++MaterialIndex)
		{
			VitruvioModelComponent->SetMaterial(MaterialIndex, VitruvioModelComponent->GetStaticMesh()->GetMaterial(MaterialIndex));
		}

		if (!Result.GenerateOptions.bIgnoreMaterialReplacements)
		{
			ApplyMaterialReplacements(VitruvioModelComponent, MaterialIdentifiers, MaterialReplacement);
		}
	}
	else
	{
		VitruvioModelComponent->SetStaticMesh(nullptr);
	}

	TMap<FString, int32> NameMap;
	TSet<FInstance> Replaced;

	if (!Result.GenerateOptions.bIgnoreInstanceReplacements)
	{
		Replaced = ApplyInstanceReplacements(VitruvioModelComponent, ConvertedResult.Instances, InstanceReplacement, NameMap);
	}

	for (const FInstance& Instance : ConvertedResult.Instances)
	{
		if (Replaced.Contains(Instance))
		{
			continue;
		}

		FString UniqueName = UniqueComponentName(Instance.Name, NameMap);
		auto InstancedComponent = NewObject<UGeneratedModelHISMComponent>(VitruvioModelComponent, FName(UniqueName),
																		  RF_Transient | RF_TextExportTransient | RF_DuplicateTransient);
		
		UStaticMesh* StaticMesh = Instance.InstanceMesh->GetStaticMesh();
		InstancedComponent->SetStaticMesh(StaticMesh);
		InstancedComponent->SetMeshIdentifier(Instance.InstanceMesh->GetIdentifier());
		InstancedComponent->RecreatePhysicsState();

		// Add all instance transforms
		const TArray<FTransform>& Transforms = Instance.Transforms;
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
		InitialShapeSceneComponent->GetOwner()->AddOwnedComponent(InstancedComponent);
		InstancedComponent->OnComponentCreated();
		InstancedComponent->RegisterComponent();

		if (!Result.GenerateOptions.bIgnoreMaterialReplacements)
		{
			ApplyMaterialReplacements(InstancedComponent, MaterialIdentifiers, MaterialReplacement);
		}
	}

	OnHierarchyChanged.Broadcast(this);

	bHasGeneratedModel = true;

	SetInitialShapeVisible(!HideAfterGeneration);

	if (Result.CallbackProxy)
	{
		Result.CallbackProxy->OnGenerateCompletedBlueprint.Broadcast();
		Result.CallbackProxy->OnGenerateCompleted.Broadcast();
		Result.CallbackProxy->SetReadyToDestroy();
	}
	OnGenerateCompleted.Broadcast();
}

void UVitruvioComponent::ProcessAttributesEvaluationQueue()
{
	if (!AttributesEvaluationQueue.IsEmpty())
	{
		FAttributesEvaluationQueueItem AttributesEvaluation;
		AttributesEvaluationQueue.Dequeue(AttributesEvaluation);

		AttributesEvaluation.AttributeMap->UpdateUnrealAttributeMap(Attributes, this);

		bAttributesReady = true;
		bNotifyAttributeChange = true;

		if (AttributesEvaluation.CallbackProxy)
		{
			AttributesEvaluation.CallbackProxy->OnAttributesEvaluatedBlueprint.Broadcast();
			AttributesEvaluation.CallbackProxy->OnAttributesEvaluated.Broadcast();
		}
		OnAttributesEvaluated.Broadcast();

		if (AttributesEvaluation.bForceRegenerate)
		{
			Generate(AttributesEvaluation.CallbackProxy);
		}
		else if (AttributesEvaluation.CallbackProxy)
		{
			AttributesEvaluation.CallbackProxy->SetReadyToDestroy();
		}
	}
}

void UVitruvioComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	if (!bInitialized)
	{
		Initialize();
	}

	ProcessGenerateQueue();
	ProcessAttributesEvaluationQueue();

	if (bNotifyAttributeChange)
	{
		NotifyAttributesChanged();
		bNotifyAttributeChange = false;
	}
}

void UVitruvioComponent::NotifyAttributesChanged()
{
#if WITH_EDITOR
	// Notify possible listeners (eg. Details panel) about changes to the Attributes
	FPropertyChangedEvent PropertyEvent(GetClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UVitruvioComponent, Attributes)));
	OnAttributesChanged.Broadcast(this, PropertyEvent);
#endif
}

void UVitruvioComponent::RemoveGeneratedMeshes()
{
	if (!InitialShape || !InitialShapeSceneComponent)
	{
		return;
	}

	TArray<USceneComponent*> Children;
	InitialShapeSceneComponent->GetChildrenComponents(true, Children);
	for (USceneComponent* Child : Children)
	{
		Child->DestroyComponent();
	}

	bHasGeneratedModel = false;
	SetInitialShapeVisible(true);
}

bool UVitruvioComponent::GetAttributesReady() const
{
	return bAttributesReady;
}

bool UVitruvioComponent::HasGeneratedModel() const
{
	return bHasGeneratedModel;
}

UGeneratedModelStaticMeshComponent* UVitruvioComponent::GetGeneratedModelComponent() const
{
	if (!InitialShapeSceneComponent)
	{
		return nullptr;
	}

	UGeneratedModelStaticMeshComponent* VitruvioModelComponent = nullptr;

	TArray<USceneComponent*> InitialShapeChildComponents;
	InitialShapeSceneComponent->GetChildrenComponents(false, InitialShapeChildComponents);
	for (USceneComponent* Component : InitialShapeChildComponents)
	{
		if (Component->IsA(UGeneratedModelStaticMeshComponent::StaticClass()))
		{
			VitruvioModelComponent = Cast<UGeneratedModelStaticMeshComponent>(Component);
			break;
		}
	}

	return VitruvioModelComponent;
}

TArray<UGeneratedModelHISMComponent*> UVitruvioComponent::GetGeneratedModelHISMComponents() const
{
	if (!InitialShapeSceneComponent)
	{
		return {};
	}

	TArray<UGeneratedModelHISMComponent*> VitruvioModelHISMComponents;
	TArray<USceneComponent*> InitialShapeChildComponents;

	InitialShapeSceneComponent->GetChildrenComponents(true, InitialShapeChildComponents);
	for (USceneComponent* Component : InitialShapeChildComponents)
	{
		if (Component->IsA(UGeneratedModelHISMComponent::StaticClass()))
		{
			VitruvioModelHISMComponents.Add(Cast<UGeneratedModelHISMComponent>(Component));
		}
	}

	return VitruvioModelHISMComponents;
}

FString UVitruvioComponent::GetMaterialIdentifier(const UMaterialInterface* SourceMaterial) const
{
	if (const FString* Result = MaterialIdentifiers.Find(SourceMaterial); ensure(Result))
	{
		return *Result;
	}

	return {};
}

void UVitruvioComponent::OnComponentDestroyed(bool bDestroyingHierarchy)
{
	if (GenerateToken)
	{
		GenerateToken->Invalidate();
	}

	if (EvalAttributesInvalidationToken)
	{
		EvalAttributesInvalidationToken->Invalidate();
	}

	VitruvioModule::Get().InvalidateOcclusionHandle(InitialShapeIndex);

#if WITH_EDITOR
	FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(PropertyChangeDelegate);
	PropertyChangeDelegate.Reset();

	if (GEngine)
	{
		GEngine->OnActorMoved().Remove(OnActorMoved);
		GEngine->OnActorsMoved().Remove(OnActorsMoved);
	}
#endif
}

void UVitruvioComponent::Generate(UGenerateCompletedCallbackProxy* CallbackProxy, const FGenerateOptions& GenerateOptions)
{
	Initialize();
	
	VitruvioModule::Get().InvalidateOcclusionHandle(InitialShapeIndex);

	// Since we can not abort an ongoing generate call from PRT, we invalidate the result and regenerate after the current generate call has
	// completed.
	if (GenerateToken)
	{
		GenerateToken->Invalidate();
		GenerateToken.Reset();
	}

	if (bBatchGenerate)
	{
		UVitruvioBatchSubsystem* BatchGenerateSubsystem = GetWorld()->GetSubsystem<UVitruvioBatchSubsystem>();
		BatchGenerateSubsystem->Generate(this, CallbackProxy);

		return;
	}

	if (!HasValidInputData())
	{
		RemoveGeneratedMeshes();
		return;
	}

	if (InitialShape)
	{
		TArray<FInitialShape> Shapes;
		
		Shapes.Add( GetInitialShape() );
	
		if (bEnableOcclusionQueries)
		{
			Shapes.Append(GetNeighboringShapes());
		}
		
		FGenerateResult GenerateResult = VitruvioModule::Get().GenerateAsync(MoveTemp(Shapes));

		GenerateToken = GenerateResult.Token;

		// clang-format off
		GenerateResult.Result.Next([this, CallbackProxy, GenerateOptions](const FGenerateResult::ResultType& Result)
		{
			FScopeLock Lock(&Result.Token->Lock);

			if (Result.Token->IsInvalid())
			{
				return;
			}

			GenerateToken.Reset();
			GenerateQueue.Enqueue({Result.Value, GenerateOptions, CallbackProxy});
		});
		// clang-format on
	}
}

#if WITH_EDITOR

void UVitruvioComponent::PreEditUndo()
{
	Super::PreEditUndo();

	DestroyInitialShapeComponent();
}

void UVitruvioComponent::PostUndoRedo()
{
	DestroyInitialShapeComponent();
	InitializeInitialShapeComponent();

	if (!PropertyChangeDelegate.IsValid())
	{
		PropertyChangeDelegate = FCoreUObjectDelegates::OnObjectPropertyChanged.AddUObject(this, &UVitruvioComponent::OnPropertyChanged);
	}

	Generate();
}

void UVitruvioComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	OnPropertyChanged(this, PropertyChangedEvent);
}

void UVitruvioComponent::OnPropertyChanged(UObject* Object, FPropertyChangedEvent& PropertyChangedEvent)
{
	if (Object != this && !IsOuterOf(Object, this->GetOwner()))
	{
		return;
	}
	bool bComponentPropertyChanged = false;

	if (PropertyChangedEvent.Property != nullptr)
	{
		if (PropertyChangedEvent.Property->GetFName() == GET_MEMBER_NAME_CHECKED(UVitruvioComponent, bBatchGenerate))
		{
			bComponentPropertyChanged = true;

			if (GenerateToken)
			{
				GenerateToken->Invalidate();
				GenerateToken.Reset();
			}

			UVitruvioBatchSubsystem* VitruvioSubsystem = GetWorld()->GetSubsystem<UVitruvioBatchSubsystem>();

			if (bBatchGenerate)
			{
				RemoveGeneratedMeshes();
				VitruvioSubsystem->RegisterVitruvioComponent(this);
			}
			else
			{
				VitruvioSubsystem->UnregisterVitruvioComponent(this);
			}
		}
		
		if (PropertyChangedEvent.Property->GetFName() == GET_MEMBER_NAME_CHECKED(UVitruvioComponent, Rpk))
		{
			Attributes.Empty();
			bAttributesReady = false;
			bComponentPropertyChanged = true;
			bNotifyAttributeChange = true;
		}

		if (PropertyChangedEvent.Property->GetFName() == GET_MEMBER_NAME_CHECKED(UVitruvioComponent, RandomSeed))
		{
			bValidRandomSeed = true;
			bComponentPropertyChanged = true;
		}

		if (PropertyChangedEvent.Property->GetFName() == GET_MEMBER_NAME_CHECKED(UVitruvioComponent, HideAfterGeneration))
		{
			SetInitialShapeVisible(!(HideAfterGeneration && bHasGeneratedModel));
		}

		if (PropertyChangedEvent.Property->GetFName() == GET_MEMBER_NAME_CHECKED(UVitruvioComponent, GenerateAutomatically))
		{
			bComponentPropertyChanged = true;
		}

		if (PropertyChangedEvent.Property->GetFName() == GET_MEMBER_NAME_CHECKED(UVitruvioComponent, bEnableOcclusionQueries))
		{
			bComponentPropertyChanged = true;
		}

		if (PropertyChangedEvent.Property->GetFName() == GET_MEMBER_NAME_CHECKED(UVitruvioComponent, MaterialReplacement))
		{
			bComponentPropertyChanged = true;
		}

		if (PropertyChangedEvent.Property->GetFName() == GET_MEMBER_NAME_CHECKED(UVitruvioComponent, InstanceReplacement))
		{
			bComponentPropertyChanged = true;
		}
	}

	// If an object was changed via an undo command, the PropertyChangedEvent.Property is null
	// Therefore, we can only check the ObjectType and check if the Property is null (=likely undo event)
	// This is suboptimal, since it can also happen during undo commands on irrelevant properties of that object. Might be improved later...
	const bool bIsSplinePropertyUndo = Object->IsA(USplineComponent::StaticClass()) && PropertyChangedEvent.Property == nullptr;
	const bool bIsAttributeUndo = Object->IsA(URuleAttribute::StaticClass()) && PropertyChangedEvent.Property == nullptr;

	const bool bRelevantProperty =
		InitialShape != nullptr && (bIsSplinePropertyUndo || InitialShape->IsRelevantProperty(Object, PropertyChangedEvent));
	const bool bRecreateInitialShape = IsRelevantObject(this, Object) && bRelevantProperty;

	// If a property has changed which is used by the initial shape we have to update it's polygon
	if (bRecreateInitialShape)
	{
		Modify();
		InitialShape->UpdatePolygon(this);

		CalculateRandomSeed();
	}

	const bool bGenerateComponent = bAttributesReady && GenerateAutomatically && (bRecreateInitialShape || bComponentPropertyChanged);
	const bool bGenerateBatch = bBatchGenerate && (bRecreateInitialShape || bComponentPropertyChanged);

	if (bGenerateComponent || bGenerateBatch)
	{
		Generate();
	}

	if (!bBatchGenerate)
	{
		if (!HasValidInputData())
		{
			RemoveGeneratedMeshes();
		}

		if (HasValidInputData() && (!bAttributesReady || bIsAttributeUndo))
		{
			EvaluateRuleAttributes(true);
		}
	}
}

void UVitruvioComponent::SetInitialShapeType(const TSubclassOf<UInitialShape>& Type)
{
	RemoveGeneratedMeshes();

	UInitialShape* NewInitialShape = NewObject<UInitialShape>(GetOwner(), Type, NAME_None, RF_Transactional);

	if (InitialShape)
	{
		if (InitialShape->GetClass() == Type)
		{
			return;
		}

		NewInitialShape->SetPolygon(InitialShape->GetPolygon());

		InitialShape->Rename(nullptr, GetTransientPackage()); // Remove from Owner
		InitialShape = NewInitialShape;

		DestroyInitialShapeComponent();
		InitializeInitialShapeComponent();
	}
	else
	{
		InitialShape = NewInitialShape;
		InitializeInitialShapeComponent();
	}
}

#endif // WITH_EDITOR

void UVitruvioComponent::EvaluateRuleAttributes(bool ForceRegenerate, UGenerateCompletedCallbackProxy* CallbackProxy)
{
	Initialize();
	
	if (EvalAttributesInvalidationToken)
	{
		EvalAttributesInvalidationToken->Invalidate();
		EvalAttributesInvalidationToken.Reset();
	}
	
	if (bBatchGenerate)
	{
		UVitruvioBatchSubsystem* BatchGenerateSubsystem = GetWorld()->GetSubsystem<UVitruvioBatchSubsystem>();

		if (ForceRegenerate)
		{
			BatchGenerateSubsystem->Generate(this, CallbackProxy);
		}
		else
		{
			BatchGenerateSubsystem->EvaluateAttributes(this, CallbackProxy);
		}

		return;
	}

	if (!HasValidInputData())
	{
		return;
	}

	bAttributesReady = false;

	FAttributeMapResult AttributesResult = VitruvioModule::Get().EvaluateRuleAttributesAsync(GetInitialShape());

	EvalAttributesInvalidationToken = AttributesResult.Token;

	AttributesResult.Result.Next([this, CallbackProxy, ForceRegenerate](const FAttributeMapResult::ResultType& Result) {
		FScopeLock Lock(&Result.Token->Lock);

		if (Result.Token->IsInvalid())
		{
			return;
		}

		EvalAttributesInvalidationToken.Reset();
		AttributesEvaluationQueue.Enqueue({Result.Value, ForceRegenerate, CallbackProxy});
	});
}

void UVitruvioComponent::InitializeInitialShapeComponent()
{
	if (InitialShapeSceneComponent)
	{
		return;
	}

	InitialShapeSceneComponent = InitialShape->CreateInitialShapeComponent(this);
	InitialShape->UpdateSceneComponent(this);
}

void UVitruvioComponent::DestroyInitialShapeComponent()
{
	if (!InitialShapeSceneComponent)
	{
		return;
	}

	// Similarly to Unreal Ed component deletion. See ComponentEditorUtils#DeleteComponents
#if WITH_EDITOR
	Modify();
#endif

	// Note that promote to children of DestroyComponent only checks for attached children not actual child components
	// therefore we have to destroy them manually here
	TArray<USceneComponent*> Children;
	InitialShapeSceneComponent->GetChildrenComponents(true, Children);
	for (USceneComponent* Child : Children)
	{
		Child->DestroyComponent(true);
	}

	AActor* Owner = InitialShapeSceneComponent->GetOwner();

	InitialShapeSceneComponent->DestroyComponent();
#if WITH_EDITOR
	Owner->RerunConstructionScripts();
#endif

	InitialShapeSceneComponent = nullptr;
}

TArray<TSubclassOf<UInitialShape>> UVitruvioComponent::GetInitialShapesClasses()
{
	return {UStaticMeshInitialShape::StaticClass(), USplineInitialShape::StaticClass()};
}
