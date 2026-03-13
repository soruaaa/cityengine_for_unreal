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

#include "GenerateCompletedCallbackProxy.h"

#include "VitruvioBatchSubsystem.h"
#include "Algo/Count.h"
#include "Engine/World.h"
#include "VitruvioBlueprintLibrary.h"

namespace
{
void CopyInitialShapeSceneComponent(AActor* OldActor, AActor* NewActor)
{
	for (const auto& InitialShapeClasses : UVitruvioComponent::GetInitialShapesClasses())
	{
		UInitialShape* DefaultInitialShape = Cast<UInitialShape>(InitialShapeClasses->GetDefaultObject());
		if (DefaultInitialShape && DefaultInitialShape->CanConstructFrom(OldActor))
		{
			DefaultInitialShape->CopySceneComponent(OldActor, NewActor);
		}

		break;
	}
}

template <typename TFun>
UGenerateCompletedCallbackProxy* ExecuteIfComponentValid(const FString& FunctionName, UVitruvioComponent* VitruvioComponent, TFun&& Function)
{
	UGenerateCompletedCallbackProxy* Proxy = NewObject<UGenerateCompletedCallbackProxy>();
	if (VitruvioComponent)
	{
		Proxy->RegisterWithGameInstance(VitruvioComponent);
		Function(Proxy, VitruvioComponent);
	}
	else
	{
		UE_LOG(LogVitruvioComponent, Error, TEXT("Cannot execute \"%s\" without valid VitruvioComponent argument."), *FunctionName)
	}

	return Proxy;
}

} // namespace

UGenerateCompletedCallbackProxy* UGenerateCompletedCallbackProxy::SetRpk(UVitruvioComponent* VitruvioComponent, URulePackage* RulePackage,
																		 bool bEvaluateAttributes, bool bGenerateModel)
{
	return ExecuteIfComponentValid(TEXT("SetRpk"), VitruvioComponent, [RulePackage, bGenerateModel](UGenerateCompletedCallbackProxy* Proxy, UVitruvioComponent* VitruvioComponent)
	{
		VitruvioComponent->SetRpk(RulePackage, true, bGenerateModel, Proxy);
	});
}

UGenerateCompletedCallbackProxy* UGenerateCompletedCallbackProxy::SetRandomSeed(UVitruvioComponent* VitruvioComponent, int32 NewRandomSeed,
																				bool bEvaluateAttributes, bool bGenerateModel)
{
	return ExecuteIfComponentValid(TEXT("SetRandomSeed"), VitruvioComponent, [NewRandomSeed, bGenerateModel, bEvaluateAttributes](UGenerateCompletedCallbackProxy* Proxy, UVitruvioComponent* VitruvioComponent)
	{
		VitruvioComponent->SetRandomSeed(NewRandomSeed, bEvaluateAttributes, bGenerateModel, Proxy);
	});
}

UGenerateCompletedCallbackProxy* UGenerateCompletedCallbackProxy::Generate(UVitruvioComponent* VitruvioComponent, FGenerateOptions GenerateOptions)
{
	return ExecuteIfComponentValid(TEXT("Generate"), VitruvioComponent, [GenerateOptions](UGenerateCompletedCallbackProxy* Proxy, UVitruvioComponent* VitruvioComponent)
	{
		VitruvioComponent->Generate(Proxy, GenerateOptions);
	});
}

UGenerateCompletedCallbackProxy* UGenerateCompletedCallbackProxy::SetFloatAttribute(UVitruvioComponent* VitruvioComponent, const FString& Name,
	float Value, bool bEvaluateAttributes, bool bGenerateModel)
{
	return ExecuteIfComponentValid(TEXT("SetFloatAttribute"), VitruvioComponent, [&Name, &Value, bEvaluateAttributes, bGenerateModel](UGenerateCompletedCallbackProxy* Proxy, UVitruvioComponent* VitruvioComponent)
	{
		VitruvioComponent->SetFloatAttribute(Name, Value, bEvaluateAttributes, bGenerateModel, Proxy);
	});
}

UGenerateCompletedCallbackProxy* UGenerateCompletedCallbackProxy::SetStringAttribute(UVitruvioComponent* VitruvioComponent, const FString& Name,
	const FString& Value, bool bEvaluateAttributes, bool bGenerateModel)
{
	return ExecuteIfComponentValid(TEXT("SetStringAttribute"), VitruvioComponent, [&Name, &Value, bEvaluateAttributes, bGenerateModel](UGenerateCompletedCallbackProxy* Proxy, UVitruvioComponent* VitruvioComponent)
	{
		VitruvioComponent->SetStringAttribute(Name, Value, bEvaluateAttributes, bGenerateModel, Proxy);
	});
}

UGenerateCompletedCallbackProxy* UGenerateCompletedCallbackProxy::SetBoolAttribute(UVitruvioComponent* VitruvioComponent, const FString& Name,
	bool Value, bool bEvaluateAttributes, bool bGenerateModel)
{
	return ExecuteIfComponentValid(TEXT("SetBoolAttribute"), VitruvioComponent, [&Name, &Value, bEvaluateAttributes, bGenerateModel](UGenerateCompletedCallbackProxy* Proxy, UVitruvioComponent* VitruvioComponent)
	{
		VitruvioComponent->SetBoolAttribute(Name, Value, bEvaluateAttributes, bGenerateModel, Proxy);
	});
}

UGenerateCompletedCallbackProxy* UGenerateCompletedCallbackProxy::SetFloatArrayAttribute(UVitruvioComponent* VitruvioComponent, const FString& Name,
	const TArray<double>& Values, bool bEvaluateAttributes, bool bGenerateModel)
{
	return ExecuteIfComponentValid(TEXT("SetFloatArrayAttribute"), VitruvioComponent, [&Name, &Values, bEvaluateAttributes, bGenerateModel](UGenerateCompletedCallbackProxy* Proxy, UVitruvioComponent* VitruvioComponent)
	{
		VitruvioComponent->SetFloatArrayAttribute(Name, Values, bEvaluateAttributes, bGenerateModel, Proxy);
	});
}

UGenerateCompletedCallbackProxy* UGenerateCompletedCallbackProxy::SetStringArrayAttribute(UVitruvioComponent* VitruvioComponent, const FString& Name,
	const TArray<FString>& Values, bool bEvaluateAttributes, bool bGenerateModel)
{
	return ExecuteIfComponentValid(TEXT("SetStringArrayAttribute"), VitruvioComponent, [&Name, &Values, bEvaluateAttributes, bGenerateModel](UGenerateCompletedCallbackProxy* Proxy, UVitruvioComponent* VitruvioComponent)
	{
		VitruvioComponent->SetStringArrayAttribute(Name, Values, bEvaluateAttributes, bGenerateModel, Proxy);
	});
}

UGenerateCompletedCallbackProxy* UGenerateCompletedCallbackProxy::SetBoolArrayAttribute(UVitruvioComponent* VitruvioComponent, const FString& Name,
	const TArray<bool>& Values, bool bEvaluateAttributes, bool bGenerateModel)
{
	return ExecuteIfComponentValid(TEXT("SetBoolArrayAttribute"), VitruvioComponent, [&Name, &Values, bEvaluateAttributes, bGenerateModel](UGenerateCompletedCallbackProxy* Proxy, UVitruvioComponent* VitruvioComponent)
	{
		VitruvioComponent->SetBoolArrayAttribute(Name, Values, bEvaluateAttributes, bGenerateModel, Proxy);
	});
}

UGenerateCompletedCallbackProxy* UGenerateCompletedCallbackProxy::SetAttributes(UVitruvioComponent* VitruvioComponent,
	const TMap<FString, FString>& NewAttributes, bool bEvaluateAttributes, bool bGenerateModel)
{
	return ExecuteIfComponentValid(TEXT("SetAttributes"), VitruvioComponent, [&NewAttributes, bEvaluateAttributes, bGenerateModel](UGenerateCompletedCallbackProxy* Proxy, UVitruvioComponent* VitruvioComponent)
	{
		VitruvioComponent->SetAttributes(NewAttributes, bEvaluateAttributes, bGenerateModel, Proxy);
	});
}

UGenerateCompletedCallbackProxy* UGenerateCompletedCallbackProxy::SetMeshInitialShape(UVitruvioComponent* VitruvioComponent, UStaticMesh* StaticMesh,
	bool bEvaluateAttributes, bool bGenerateModel)
{
	return ExecuteIfComponentValid(TEXT("SetMeshInitialShape"), VitruvioComponent, [StaticMesh, bEvaluateAttributes, bGenerateModel](UGenerateCompletedCallbackProxy* Proxy, UVitruvioComponent* VitruvioComponent)
	{
		VitruvioComponent->SetMeshInitialShape(StaticMesh, bEvaluateAttributes, bGenerateModel, Proxy);
	});
}

UGenerateCompletedCallbackProxy* UGenerateCompletedCallbackProxy::SetSplineInitialShape(UVitruvioComponent* VitruvioComponent,
	const TArray<FSplinePoint>& SplinePoints, bool bEvaluateAttributes, bool bGenerateModel)
{
	return ExecuteIfComponentValid(TEXT("SetSplineInitialShape"), VitruvioComponent, [&SplinePoints, bEvaluateAttributes, bGenerateModel](UGenerateCompletedCallbackProxy* Proxy, UVitruvioComponent* VitruvioComponent)
	{
		VitruvioComponent->SetSplineInitialShape(SplinePoints, bEvaluateAttributes, bGenerateModel, Proxy);
	});
}

UGenerateCompletedCallbackProxy* UGenerateCompletedCallbackProxy::SetPolygonInitialShape(UVitruvioComponent* VitruvioComponent, const FInitialShapePolygon& InitialShapePolygon,
	bool bEvaluateAttributes, bool bGenerateModel)
{
	return ExecuteIfComponentValid(TEXT("SetSplineInitialShape"), VitruvioComponent, [InitialShapePolygon, bEvaluateAttributes, bGenerateModel](UGenerateCompletedCallbackProxy* Proxy, UVitruvioComponent* VitruvioComponent)
	{
		VitruvioComponent->SetPolygonInitialShape(InitialShapePolygon, bEvaluateAttributes, bGenerateModel, Proxy);
	});
}

UGenerateCompletedCallbackProxy* UGenerateCompletedCallbackProxy::ConvertToVitruvioActor(UObject* WorldContextObject, const TArray<AActor*>& Actors,
                                                                                         TArray<AVitruvioActor*>& OutVitruvioActors,
                                                                                         URulePackage* Rpk, bool bGenerateModels, bool bBatchGeneration)
{
	UGenerateCompletedCallbackProxy* Proxy = NewObject<UGenerateCompletedCallbackProxy>();
	Proxy->RegisterWithGameInstance(WorldContextObject);

	UGenerateCompletedCallbackProxy* NonBatchedProxy = nullptr;
	if (!bBatchGeneration)
	{
		NonBatchedProxy = NewObject<UGenerateCompletedCallbackProxy>();
		const int32 TotalActors = Algo::CountIf(Actors, [](AActor* Actor) { return UVitruvioBlueprintLibrary::CanConvertToVitruvioActor(Actor); });
		NonBatchedProxy->RegisterWithGameInstance(WorldContextObject);
		NonBatchedProxy->OnGenerateCompleted.AddLambda(FExecuteAfterCountdown(TotalActors, [Proxy]() {
			Proxy->OnGenerateCompletedBlueprint.Broadcast();
			Proxy->OnGenerateCompleted.Broadcast();
		}));
		NonBatchedProxy->OnAttributesEvaluated.AddLambda(FExecuteAfterCountdown(TotalActors, [Proxy]() {
			Proxy->OnAttributesEvaluatedBlueprint.Broadcast();
			Proxy->OnAttributesEvaluated.Broadcast();
		}));
	}

	for (AActor* Actor : Actors)
	{
		AActor* OldAttachParent = Actor->GetAttachParentActor();
		if (UVitruvioBlueprintLibrary::CanConvertToVitruvioActor(Actor))
		{
			AVitruvioActor* VitruvioActor = Actor->GetWorld()->SpawnActor<AVitruvioActor>(Actor->GetActorLocation(), Actor->GetActorRotation());

			CopyInitialShapeSceneComponent(Actor, VitruvioActor);

			UVitruvioComponent* VitruvioComponent = VitruvioActor->VitruvioComponent;
			VitruvioComponent->SetBatchGenerated(bBatchGeneration);

			VitruvioComponent->SetRpk(Rpk, !bBatchGeneration, bGenerateModels, NonBatchedProxy);

			if (OldAttachParent)
			{
				VitruvioActor->AttachToActor(OldAttachParent, FAttachmentTransformRules::KeepWorldTransform);
			}

			Actor->Destroy();

			OutVitruvioActors.Add(VitruvioActor);
		}
	}

	if (bBatchGeneration)
	{
		UVitruvioBatchSubsystem* VitruvioBatchSubsystem = WorldContextObject->GetWorld()->GetSubsystem<UVitruvioBatchSubsystem>();
		VitruvioBatchSubsystem->GenerateAll(Proxy);
	}
	
	return Proxy;
}
