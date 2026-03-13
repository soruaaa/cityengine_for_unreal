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

#include "Util/AttributeConversion.h"

#include "AnnotationParsing.h"

#include "PRTTypes.h"
#include "PRTUtils.h"
#include "RuleAttributes.h"
#include "Misc/DefaultValueHelper.h"

namespace
{
const FString DEFAULT_STYLE = TEXT("Default");

std::vector<const wchar_t*> ToPtrVector(const TArray<FString>& Input)
{
	std::vector<const wchar_t*> PtrVec(Input.Num());
	for (size_t i = 0; i < Input.Num(); i++)
		PtrVec[i] = *Input[i];
	return PtrVec;
}

URuleAttribute* CreateAttribute(const AttributeMapUPtr& AttributeMap, const prt::RuleFileInfo::Entry* AttrInfo, UObject* const Outer)
{
	const std::wstring Name(AttrInfo->getName());
	const FName AttributeName = WCHAR_TO_TCHAR(Name.c_str());
	switch (AttrInfo->getReturnType())
	{
	case prt::AAT_BOOL:
	{
		UBoolAttribute* BoolAttribute = NewObject<UBoolAttribute>(Outer);
		BoolAttribute->Value = AttributeMap->getBool(Name.c_str());
		BoolAttribute->SetFlags(RF_Transactional);
		return BoolAttribute;
	}
	case prt::AAT_INT:
	case prt::AAT_FLOAT:
	{
		UFloatAttribute* FloatAttribute = NewObject<UFloatAttribute>(Outer);
		FloatAttribute->Value = AttributeMap->getFloat(Name.c_str());
		FloatAttribute->SetFlags(RF_Transactional);
		return FloatAttribute;
	}
	case prt::AAT_STR:
	{
		UStringAttribute* StringAttribute = NewObject<UStringAttribute>(Outer);
		StringAttribute->Value = WCHAR_TO_TCHAR(AttributeMap->getString(Name.c_str()));
		StringAttribute->SetFlags(RF_Transactional);
		return StringAttribute;
	}
	case prt::AAT_STR_ARRAY:
	{
		UStringArrayAttribute* StringArrayAttribute = NewObject<UStringArrayAttribute>(Outer);
		size_t Count = 0;
		const wchar_t* const* Arr = AttributeMap->getStringArray(Name.c_str(), &Count);
		for (size_t Index = 0; Index < Count; Index++)
		{
			StringArrayAttribute->Values.Add(Arr[Index]);
		}
		StringArrayAttribute->SetFlags(RF_Transactional);
		return StringArrayAttribute;
	}
	case prt::AAT_BOOL_ARRAY:
	{
		UBoolArrayAttribute* BoolArrayAttribute = NewObject<UBoolArrayAttribute>(Outer);
		size_t Count = 0;
		const bool* Arr = AttributeMap->getBoolArray(Name.c_str(), &Count);
		for (size_t Index = 0; Index < Count; Index++)
		{
			BoolArrayAttribute->Values.Add(Arr[Index]);
		}
		BoolArrayAttribute->SetFlags(RF_Transactional);
		return BoolArrayAttribute;
	}
	case prt::AAT_FLOAT_ARRAY:
	{
		UFloatArrayAttribute* FloatArrayAttribute = NewObject<UFloatArrayAttribute>(Outer);
		size_t Count = 0;
		const double* Arr = AttributeMap->getFloatArray(Name.c_str(), &Count);
		for (size_t Index = 0; Index < Count; Index++)
		{
			FloatArrayAttribute->Values.Add(Arr[Index]);
		}
		FloatArrayAttribute->SetFlags(RF_Transactional);
		return FloatArrayAttribute;
	}
	case prt::AAT_UNKNOWN:
	case prt::AAT_VOID:

	default:
		return nullptr;
	}
}

struct FGroupOrderKey
{
	TArray<FString> Groups;
	FString ImportPath;

	explicit FGroupOrderKey(const URuleAttribute& Attribute)
	{
		Groups = Attribute.Groups;
		ImportPath = Attribute.ImportPath;
	}

	friend bool operator==(const FGroupOrderKey& A, const FGroupOrderKey& B)
	{
		const bool bGroupsEqual = A.Groups == B.Groups;
		const bool bImportPathEqual = A.ImportPath.Equals(B.ImportPath);
		return bGroupsEqual && bImportPathEqual;
	}

	friend bool operator!=(const FGroupOrderKey& Lhs, const FGroupOrderKey& RHS)
	{
		return !(Lhs == RHS);
	}

	friend uint32 GetTypeHash(const FGroupOrderKey& Object)
	{
		uint32 Hash = 0x844310C5;
		for (const auto& Group : Object.Groups)
		{
			Hash = HashCombine(Hash, GetTypeHash(Group));
		}
		return HashCombine(Hash, GetTypeHash(Object.ImportPath));
	}
};

constexpr int AttributeGroupOrderNone = INT32_MAX;

// maps the highest attribute order from all attributes within a group to its group string key
TMap<FGroupOrderKey, int> GetGlobalGroupOrderMap(const TMap<FString, URuleAttribute*>& Attributes)
{
	TMap<FGroupOrderKey, int> GlobalGroupOrderMap;

	for (const auto& AttributeTuple : Attributes)
	{
		URuleAttribute* Attribute = AttributeTuple.Value;
		TArray<FString> CurrGroups;
		for (const FString& CurrGroup : Attribute->Groups)
		{
			CurrGroups.Add(CurrGroup);

			int& ValueRef = GlobalGroupOrderMap.FindOrAdd(FGroupOrderKey(*Attribute), AttributeGroupOrderNone);
			ValueRef = FMath::Min(ValueRef, Attribute->GroupOrder);
		}
	}
	return GlobalGroupOrderMap;
}

bool IsAttributeBeforeOther(const URuleAttribute& Attribute, const URuleAttribute& OtherAttribute,
                            const TMap<FGroupOrderKey, int> GlobalGroupOrderMap)
{
	auto AreImportPathsInOrder = [](const URuleAttribute& A, const URuleAttribute& B)
	{
		// sort main rule attributes before the rest
		if (A.ImportPath.Len() == 0 && B.ImportPath.Len() > 0)
		{
			return true;
		}

		if (B.ImportPath.Len() == 0 && A.ImportPath.Len() > 0)
		{
			return false;
		}

		if (A.ImportOrder != B.ImportOrder)
		{
			return A.ImportOrder < B.ImportOrder;
		}

		return A.ImportPath.Compare(B.ImportPath, ESearchCase::CaseSensitive) < 0;
	};

	auto IsChildOf = [](const URuleAttribute& Child, const URuleAttribute& Parent)
	{
		const size_t ParentGroupNum = Parent.Groups.Num();
		const size_t ChildGroupNum = Child.Groups.Num();

		// parent path must be shorter
		if (ParentGroupNum >= ChildGroupNum)
		{
			return false;
		}

		// parent and child paths must be identical
		for (size_t i = 0; i < ParentGroupNum; i++)
		{
			if (Parent.Groups[i] != Child.Groups[i])
			{
				return false;
			}
		}
		return true;
	};

	auto GetGlobalGroupOrder = [&GlobalGroupOrderMap](const URuleAttribute& RuleAttribute)
	{
		const int* GroupOrderPtr = GlobalGroupOrderMap.Find(FGroupOrderKey(RuleAttribute));
		return (GroupOrderPtr == nullptr) ? AttributeGroupOrderNone : (*GroupOrderPtr);
	};

	auto AreAttributeGroupsInOrder = [](const URuleAttribute& A, const URuleAttribute& B)
	{
		const size_t GroupSizeA = A.Groups.Num();
		const size_t GroupSizeB = B.Groups.Num();

		for (size_t GroupIndex = 0; GroupIndex < FMath::Max(GroupSizeA, GroupSizeB); ++GroupIndex)
		{
			// a descendant of b
			if (GroupIndex >= GroupSizeA)
				return false;

			// b descendant of a
			if (GroupIndex >= GroupSizeB)
				return true;

			// difference in groups
			if (A.Groups[GroupIndex].Equals(B.Groups[GroupIndex], ESearchCase::CaseSensitive))
				return A.Groups[GroupIndex].Compare(B.Groups[GroupIndex], ESearchCase::CaseSensitive) < 0;
		}
		return false;
	};

	auto AreAttributesWithAndWithoutGroupInOrder = [&](const URuleAttribute& RuleAttributeWithGroups,
	                                                   const URuleAttribute& RuleAttributeWithoutGroup)
	{
		if (!RuleAttributeWithGroups.Groups.IsEmpty() &&
		    (RuleAttributeWithGroups.GroupOrder == RuleAttributeWithoutGroup.Order))
			return RuleAttributeWithGroups.Groups[0].Compare(RuleAttributeWithoutGroup.DisplayName, ESearchCase::CaseSensitive) <= 0;

		return GetGlobalGroupOrder(RuleAttributeWithGroups) < RuleAttributeWithoutGroup.Order;
	};

	auto AreAttributeGroupOrdersInOrder = [&](const URuleAttribute& A, const URuleAttribute& B)
	{
		if (B.Groups.IsEmpty())
			return AreAttributesWithAndWithoutGroupInOrder(A, B);

		if (A.Groups.IsEmpty())
			return !AreAttributesWithAndWithoutGroupInOrder(B, A);

		if (IsChildOf(A, B))
		{
			return false; // child A should be sorted after parent B
		}

		if (IsChildOf(B, A))
		{
			return true; // child B should be sorted after parent A
		}

		const auto GlobalOrderA = GetGlobalGroupOrder(A);
		const auto GlobalOrderB = GetGlobalGroupOrder(B);
		if (GlobalOrderA != GlobalOrderB)
		{
			return (GlobalOrderA < GlobalOrderB);
		}

		return AreAttributeGroupsInOrder(A, B);
	};

	auto AreAttributesInOrder = [&](const URuleAttribute& A, const URuleAttribute& B)
	{
		if (A.ImportPath != B.ImportPath)
		{
			return AreImportPathsInOrder(A, B);
		}

		if (A.Groups != B.Groups)
		{
			return AreAttributeGroupOrdersInOrder(A, B);
		}

		if (A.Order == B.Order)
		{
			return A.DisplayName.Compare(B.DisplayName, ESearchCase::CaseSensitive) < 0;
		}
		return A.Order < B.Order;
	};

	return AreAttributesInOrder(Attribute, OtherAttribute);
}

template <class TAttribute, typename TValue>
requires std::is_base_of_v<URuleAttribute, TAttribute>
TAttribute* CreateScalarAttribute(const FString& Key, const TValue& InValue)
{
	TAttribute* Attr = NewObject<TAttribute>();
	Attr->Value = InValue;
	Attr->Name = Key;
	Attr->DisplayName = Key;
	Attr->bUserSet = true;
	return Attr;
}

template <class TArrayAttribute, typename TValue, typename TParseFun>
requires std::is_base_of_v<URuleAttribute, TArrayAttribute>
TArrayAttribute* CreateArrayAttributeFromValues(const FString& Key, const TArray<FString>& Elements, TParseFun&& ParseFun)
{
	TArray<TValue> ParsedValues;

	for (const FString& Element : Elements)
	{
		TValue Value;
		if (ParseFun(Element, Value))
		{
			ParsedValues.Add(Value);
		}
	}

	TArrayAttribute* Attr = NewObject<TArrayAttribute>();
	Attr->Name = Key;
	Attr->DisplayName = Key;
	Attr->bUserSet = true;
	Attr->Values = ParsedValues;
	return Attr;
}

} // namespace

namespace ParseUtils
{
bool TryParseDouble(const FString& InStr, double& OutValue)
{
	return FDefaultValueHelper::ParseDouble(InStr, OutValue);
}

bool TryParseBool(const FString& InStr, bool& OutValue)
{
	const FString Lower = InStr.ToLower();
	if (Lower == TEXT("true") || Lower == TEXT("1"))
	{
		OutValue = true;
		return true;
	}
	if (Lower == TEXT("false") || Lower == TEXT("0"))
	{
		OutValue = false;
		return true;
	}
	return false;
}

bool TryParseString(const FString& InStr, FString& OutValue)
{
	OutValue = InStr;
	return true;
}
}

namespace Vitruvio
{
void UpdateAttributeMap(TMap<FString, URuleAttribute*>& AttributeMapOut, const AttributeMapUPtr& AttributeMap, const RuleFileInfoPtr& RuleInfo,
                        UObject* const Outer)
{
	bool bNeedsResorting = false;
	TMap<FString, int> ImportOrderMap = ParseImportOrderMap(RuleInfo);

	for (size_t AttributeIndex = 0; AttributeIndex < RuleInfo->getNumAttributes(); AttributeIndex++)
	{
		const prt::RuleFileInfo::Entry* AttrInfo = RuleInfo->getAttribute(AttributeIndex);
		if (AttrInfo->getNumParameters() != 0)
		{
			continue;
		}

		// We only support the default style for the moment
		FString Style(WCHAR_TO_TCHAR(prtu::getStyle(AttrInfo->getName()).c_str()));
		if (Style != DEFAULT_STYLE)
		{
			continue;
		}

		const std::wstring Name(AttrInfo->getName());
		URuleAttribute* Attribute = ::CreateAttribute(AttributeMap, AttrInfo, Outer);

		if (Attribute)
		{
			const FString AttributeName = WCHAR_TO_TCHAR(Name.c_str());
			Attribute->Name = AttributeName;

			ParseAttributeAnnotations(AttrInfo, *Attribute, Outer);
			if (!Attribute->bHidden)
			{
				// update/add attributes if they aren't hidden
				if (AttributeMapOut.Contains(AttributeName))
				{
					const URuleAttribute* OutAttribute = AttributeMapOut[AttributeName];
					if (!OutAttribute->bUserSet)
					{
						AttributeMapOut[AttributeName]->CopyValue(Attribute);
					}
				}
				else
				{
					const FString DisplayName = WCHAR_TO_TCHAR(prtu::removeImport(prtu::removeStyle(Name.c_str())).c_str());
					const FString ImportPath = WCHAR_TO_TCHAR(prtu::getFullImportPath(Name.c_str()).c_str());

					Attribute->DisplayName = DisplayName;
					Attribute->ImportPath = ImportPath;
					int* ImportOrder = ImportOrderMap.Find(ImportPath);
					if (ImportOrder != nullptr)
					{
						Attribute->ImportOrder = *ImportOrder;
					}
					AttributeMapOut.Add(AttributeName, Attribute);
					bNeedsResorting = true;
				}
			}
		}
	}
	if (bNeedsResorting)
	{
		TMap<FGroupOrderKey, int> GlobalGroupOrder = GetGlobalGroupOrderMap(AttributeMapOut);
		AttributeMapOut.ValueSort(
			[&GlobalGroupOrder](const URuleAttribute& A, const URuleAttribute& B)
			{
				return IsAttributeBeforeOther(A, B, GlobalGroupOrder);
			});
	}
}

AttributeMapUPtr CreateAttributeMap(const TMap<FString, URuleAttribute*>& Attributes)
{
	AttributeMapBuilderUPtr AttributeMapBuilder(prt::AttributeMapBuilder::create());

	for (const TPair<FString, URuleAttribute*>& AttributeEntry : Attributes)
	{
		const URuleAttribute* Attribute = AttributeEntry.Value;

		if (!Attribute->bUserSet)
			continue;

		if (const UFloatAttribute* FloatAttribute = Cast<UFloatAttribute>(Attribute))
		{
			AttributeMapBuilder->setFloat(TCHAR_TO_WCHAR(*Attribute->Name), FloatAttribute->Value);
		}
		else if (const UStringAttribute* StringAttribute = Cast<UStringAttribute>(Attribute))
		{
			AttributeMapBuilder->setString(TCHAR_TO_WCHAR(*Attribute->Name), TCHAR_TO_WCHAR(*StringAttribute->Value));
		}
		else if (const UBoolAttribute* BoolAttribute = Cast<UBoolAttribute>(Attribute))
		{
			AttributeMapBuilder->setBool(TCHAR_TO_WCHAR(*Attribute->Name), BoolAttribute->Value);
		}
		else if (const UStringArrayAttribute* StringArrayAttribute = Cast<UStringArrayAttribute>(Attribute))
		{
			std::vector<const wchar_t*> PtrVec = ToPtrVector(StringArrayAttribute->Values);
			AttributeMapBuilder->setStringArray(TCHAR_TO_WCHAR(*Attribute->Name), PtrVec.data(), StringArrayAttribute->Values.Num());
		}
		else if (const UBoolArrayAttribute* BoolArrayAttribute = Cast<UBoolArrayAttribute>(Attribute))
		{
			AttributeMapBuilder->setBoolArray(TCHAR_TO_WCHAR(*Attribute->Name), BoolArrayAttribute->Values.GetData(),
			                                  BoolArrayAttribute->Values.Num());
		}
		else if (const UFloatArrayAttribute* FloatArrayAttribute = Cast<UFloatArrayAttribute>(Attribute))
		{
			AttributeMapBuilder->setFloatArray(TCHAR_TO_WCHAR(*Attribute->Name), FloatArrayAttribute->Values.GetData(),
			                                   FloatArrayAttribute->Values.Num());
		}
	}

	return AttributeMapUPtr(AttributeMapBuilder->createAttributeMap(), PRTDestroyer());
}

AttributeMapUPtr CreateAttributeMap(const TMap<FString, TWeakObjectPtr<URuleAttribute>>& WeakAttributes)
{
	TMap<FString, URuleAttribute*> Attributes;
	for (const auto& [Key, Attribute] : WeakAttributes)
	{
		if (Attribute.IsValid())
		{
			Attributes.Add(Key, Attribute.Get());
		}
	}
	return CreateAttributeMap(Attributes);
}

URuleAttribute* CreateAttribute(const FString& Key, const FString& Value)
{
	const FString Trimmed = Value.TrimStartAndEnd();

	if (Trimmed.StartsWith(TEXT("[")) && Trimmed.EndsWith(TEXT("]")))
	{
		FString Inner = Trimmed.Mid(1, Trimmed.Len() - 2);
		TArray<FString> Elements;
		Inner.ParseIntoArray(Elements, TEXT(","), true);

		if (Elements.Num() == 0)
		{
			return CreateArrayAttributeFromValues<UStringArrayAttribute, FString>(Key, {}, ParseUtils::TryParseString);
		}

		const FString& First = Elements[0];
		double DummyFloat;
		bool DummyBool;

		if (ParseUtils::TryParseDouble(First, DummyFloat))
		{
			return CreateArrayAttributeFromValues<UFloatArrayAttribute, double>(Key, Elements, ParseUtils::TryParseDouble);
		}
		else if (ParseUtils::TryParseBool(First, DummyBool))
		{
			return CreateArrayAttributeFromValues<UBoolArrayAttribute, bool>(Key, Elements, ParseUtils::TryParseBool);
		}
		else
		{
			return CreateArrayAttributeFromValues<UStringArrayAttribute, FString>(Key, Elements, ParseUtils::TryParseString);
		}
	}

	double ParsedFloat;
	bool ParsedBool;

	if (ParseUtils::TryParseDouble(Trimmed, ParsedFloat))
	{
		return CreateScalarAttribute<UFloatAttribute>(Key, ParsedFloat);
	}
	else if (ParseUtils::TryParseBool(Trimmed, ParsedBool))
	{
		return CreateScalarAttribute<UBoolAttribute>(Key, ParsedBool);
	}
	else
	{
		return CreateScalarAttribute<UStringAttribute>(Key, Trimmed);
	}
}
} // namespace Vitruvio
