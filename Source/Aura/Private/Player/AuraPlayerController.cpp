// Copyright PGM


#include "Player/AuraPlayerController.h"

#include "EnhancedInputSubsystems.h"
#include "Interaction/EnemyInterface.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "AbilitySystem/AuraAbilitySystemComponent.h"
#include "Input/AuraInputComponent.h"
#include "Components/SplineComponent.h"
#include "AuraGameplayTags.h"
#include "NavigationSystem.h"
#include "NavigationPath.h"
#include "GameFramework/Character.h"
#include "UI/Widget/DamageTextComponent.h"
#include "Aura/Aura.h"
#include "Kismet/GameplayStatics.h"
#include "GameFramework/HUD.h"
#include "Interaction/CombatInterface.h"

AAuraPlayerController::AAuraPlayerController()
{
	bReplicates = true;
	Spline = CreateDefaultSubobject<USplineComponent>("Spline");
}

void AAuraPlayerController::PlayerTick(float DeltaTime)
{
	Super::PlayerTick(DeltaTime);

	CursorTrace();
	AutoRun();
}

void AAuraPlayerController::ShowDamageNumber_Implementation(float DamageAmount, ACharacter* TargetCharacter, bool bBlockedHit, bool bCriticalHit)
{
	if (IsValid(TargetCharacter) && DamageTextComponentClass && IsLocalController())
	{
		UDamageTextComponent* DamageText = NewObject<UDamageTextComponent>(TargetCharacter, DamageTextComponentClass);
		DamageText->RegisterComponent();
		DamageText->AttachToComponent(TargetCharacter->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
		DamageText->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
		DamageText->SetDamageText(DamageAmount, bBlockedHit, bCriticalHit);
	}
}

void AAuraPlayerController::AutoRun()
{
	if (!bAutoRunning) return;

	APawn* ControlledPawn = GetPawn();
	if (ControlledPawn)
	{
		const FVector PawnLocation = ControlledPawn->GetActorLocation();
		const FVector LocationOnSpline = Spline->FindLocationClosestToWorldLocation(PawnLocation, ESplineCoordinateSpace::World);
		const FVector TargetSplinePointLocation = Spline->GetLocationAtSplinePoint(TargetSplinePointIdx, ESplineCoordinateSpace::World);
		FVector WorldDirection = TargetSplinePointLocation - PawnLocation;
		WorldDirection.Z = 0.0f;
		// NOTE: We get the normal after zeroing Z to get a constant movement speed along the XY plane.
		WorldDirection = WorldDirection.GetSafeNormal();
		ControlledPawn->AddMovementInput(WorldDirection);

		const float DistanceToTarget = (LocationOnSpline - TargetSplinePointLocation).Length();
		if (DistanceToTarget <= AutoRunAcceptanceRadius)
		{
			const bool bNextTargetPointExist = TargetSplinePointIdx < Spline->GetNumberOfSplinePoints() - 1;
			if (bNextTargetPointExist)
			{
				TargetSplinePointIdx++;
			}
			else
			{
				bAutoRunning = false;
			}
		}

		if (bDrawDebugEnabled)
		{
			for (int32 SplinePointIdx = 0; SplinePointIdx < Spline->GetNumberOfSplinePoints(); ++SplinePointIdx)
			{
				const FVector SplinePointLocation = Spline->GetLocationAtSplinePoint(SplinePointIdx, ESplineCoordinateSpace::World);
				if (SplinePointIdx > 0)
				{
					const FVector PreviousSplinePointLocation = Spline->GetLocationAtSplinePoint(SplinePointIdx - 1, ESplineCoordinateSpace::World);
					DrawDebugLine(GetWorld(), PreviousSplinePointLocation, SplinePointLocation, FColor::Red);
				}
				DrawDebugSphere(GetWorld(), SplinePointLocation, 10.0f, 12, FColor::Red);
			}
			DrawDebugSphere(GetWorld(), LocationOnSpline, 20.0f, 12, FColor::Cyan);

			const FVector LineStart = PawnLocation + WorldDirection.GetSafeNormal() * 50.0f;
			const FVector LineEnd = LineStart + WorldDirection * 100.0f;
			UKismetSystemLibrary::DrawDebugArrow(this, LineStart, LineEnd, 20.0f, FLinearColor::Yellow, 0.0f, 4);

			DrawDebugSphere(GetWorld(), TargetSplinePointLocation, 20.0f, 12, FColor::Yellow);

			UE_LOG(LogTemp, Warning, TEXT("TargetSplinePointIdx: %i, DistanceToTarget: %f"), TargetSplinePointIdx, DistanceToTarget);
		}
	}
}

void AAuraPlayerController::CursorTrace()
{
	GetHitResultUnderCursor(ECC_Visibility, false, CursorHit);
	if (!CursorHit.bBlockingHit) return;

	LastActor = ThisActor;
	ThisActor = CursorHit.GetActor();

	if (LastActor != ThisActor)
	{
		if (LastActor) LastActor->UnHighlightActor();
		if (ThisActor) ThisActor->HighlightActor();
	}
}

void AAuraPlayerController::AbilityInputTagPressed(FGameplayTag InputTag)
{
	if (InputTag.MatchesTagExact(FAuraGameplayTags::Get().InputTag_LMB))
	{
		ControlledPawnHalfHeight = Cast<ICombatInterface>(GetPawn())->GetHalfHeight();
		bTargeting = ThisActor ? true : false;
		bAutoRunning = false;
	}
}

void AAuraPlayerController::AbilityInputTagReleased(FGameplayTag InputTag)
{
	if (!InputTag.MatchesTagExact(FAuraGameplayTags::Get().InputTag_LMB))
	{
		if (GetASC()) GetASC()->AbilityInputTagReleased(InputTag);
		return;
	}

	if (GetASC()) GetASC()->AbilityInputTagReleased(InputTag);

	if (!bTargeting && !bShiftKeyDown)
	{
		if (FollowTime <= ShortPressThreshold)
		{
			APawn* ControlledPawn = GetPawn();
			if (ControlledPawn)
			{
				FHitResult NavChannelCursorHitResult;
				GetHitResultUnderCursor(ECC_Navigation, false, NavChannelCursorHitResult);
				if (NavChannelCursorHitResult.bBlockingHit)
				{
					FNavLocation ImpactPointNavLocation;
					const FVector QueryingExtent = FVector(400.0f, 400.0f, 250.0f);
					const FNavAgentProperties& NavAgentProps = GetNavAgentPropertiesRef();
					const bool bNavLocationFound = NavSystem->ProjectPointToNavigation(NavChannelCursorHitResult.ImpactPoint, ImpactPointNavLocation, QueryingExtent, &NavAgentProps);
					if (bNavLocationFound)
					{
						UNavigationPath* NavigationPath = UNavigationSystemV1::FindPathToLocationSynchronously(this, ControlledPawn->GetActorLocation(), ImpactPointNavLocation);
						if (NavigationPath && NavigationPath->PathPoints.Num() > 0)
						{
							Spline->ClearSplinePoints();
							for (const FVector& PathPoint : NavigationPath->PathPoints)
							{
								Spline->AddSplinePoint(PathPoint, ESplineCoordinateSpace::World);
							}
							TargetSplinePointIdx = 1;
							bAutoRunning = true;
						}
					}

					if (bDrawDebugEnabled)
					{
						DrawDebugBox(GetWorld(), NavChannelCursorHitResult.ImpactPoint, QueryingExtent, FColor::Silver, false, 3.0f);
					}
				}
			}
		}
		FollowTime = 0.0f;
		bTargeting = false;
	}
}

void AAuraPlayerController::AbilityInputTagHeld(FGameplayTag InputTag)
{
	if (!InputTag.MatchesTagExact(FAuraGameplayTags::Get().InputTag_LMB))
	{
		if (GetASC()) GetASC()->AbilityInputTagHeld(InputTag);
		return;
	}

	if (bTargeting || bShiftKeyDown)
	{
		if (GetASC()) GetASC()->AbilityInputTagHeld(InputTag);
	}
	else
	{
		FollowTime += GetWorld()->GetDeltaSeconds();

		APawn* ControlledPawn = GetPawn();
		if (ControlledPawn)
		{
			const FVector PawnLocation = ControlledPawn->GetActorLocation();
			FVector PawnBottomLocation = PawnLocation;
			PawnBottomLocation.Z -= ControlledPawnHalfHeight;
			FVector CursorHorizPlaneIntersection;
			const bool bIntersectionFound = GetCursorPlaneIntersection(PawnBottomLocation, FVector::UpVector, CursorHorizPlaneIntersection);
			if (bIntersectionFound)
			{
				FVector WorldDirection = (CursorHorizPlaneIntersection - PawnLocation).GetSafeNormal();
				WorldDirection.Z = 0.0f;
				ControlledPawn->AddMovementInput(WorldDirection);

				if (bDrawDebugEnabled)
				{
					DrawDebugSphere(GetWorld(), CursorHorizPlaneIntersection, 20.0f, 12, FColor::Green);

					const FVector LineStart = PawnLocation + WorldDirection.GetSafeNormal() * 50.0f;
					const FVector LineEnd = LineStart + WorldDirection * 100.0f;
					UKismetSystemLibrary::DrawDebugArrow(this, LineStart, LineEnd, 20.0f, FLinearColor::Green, 0.0f, 4);
				}
			}
		}


	}
}

UAuraAbilitySystemComponent* AAuraPlayerController::GetASC()
{
	if (AuraAbilitySystemComponent == nullptr)
	{
		AuraAbilitySystemComponent = Cast<UAuraAbilitySystemComponent>(UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(GetPawn<APawn>()));
	}

	return AuraAbilitySystemComponent;
}



void AAuraPlayerController::BeginPlay()
{
	Super::BeginPlay();
	check(AuraContext);

	UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer());
	if (Subsystem)
	{
		Subsystem->AddMappingContext(AuraContext, 0);
	}
	
	bShowMouseCursor = true;
	DefaultMouseCursor = EMouseCursor::Default;

	FInputModeGameAndUI InputModeData;
	InputModeData.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	InputModeData.SetHideCursorDuringCapture(false);
	SetInputMode(InputModeData);

	NavSystem = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
}

void AAuraPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	UAuraInputComponent* AuraInputComponent = CastChecked<UAuraInputComponent>(InputComponent);
	AuraInputComponent->BindAction(MoveAction, ETriggerEvent::Triggered, this, &AAuraPlayerController::Move);
	AuraInputComponent->BindAction(ShiftAction, ETriggerEvent::Started, this, &AAuraPlayerController::ShiftPressed);
	AuraInputComponent->BindAction(ShiftAction, ETriggerEvent::Completed, this, &AAuraPlayerController::ShiftReleased);
	AuraInputComponent->BindAbilityActions(InputConfig, this, &ThisClass::AbilityInputTagPressed, &ThisClass::AbilityInputTagReleased, &ThisClass::AbilityInputTagHeld);
}

void AAuraPlayerController::Move(const FInputActionValue& InputActionValue)
{
	const FVector2D InputAxisVector = InputActionValue.Get<FVector2D>();
	const FRotator Rotation = GetControlRotation();
	const FRotator YawRotation(0.f, Rotation.Yaw, 0.f);

	const FVector ForwardDirection = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::X);
	const FVector RightDirection = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::Y);

	if (APawn* ControlledPawn = GetPawn<APawn>())
	{
		ControlledPawn->AddMovementInput(ForwardDirection, InputAxisVector.Y);
		ControlledPawn->AddMovementInput(RightDirection, InputAxisVector.X);
	}
	bAutoRunning = false;
}

bool AAuraPlayerController::GetCursorPlaneIntersection(const FVector& InPlaneOrigin, const FVector& InPlaneNormal, FVector& OutPlanePoint) const
{
	ULocalPlayer* LocalPlayer = Cast<ULocalPlayer>(Player);
	if (LocalPlayer && LocalPlayer->ViewportClient)
	{
		FVector2D MousePosition;
		const bool bMousePositionFound = LocalPlayer->ViewportClient->GetMousePosition(MousePosition);
		if (bMousePositionFound)
		{
			return GetScreenPositionPlaneIntersection(MousePosition, InPlaneOrigin, InPlaneNormal, OutPlanePoint);
		}
	}

	return false;
}

bool AAuraPlayerController::GetScreenPositionPlaneIntersection(const FVector2D& ScreenPosition, const FVector& InPlaneOrigin, const FVector& InPlaneNormal, FVector& OutPlanePoint) const
{
	// Early out if we clicked on a HUD hitbox.
	AHUD* HUD = GetHUD();
	if (HUD && HUD->GetHitBoxAtCoordinates(ScreenPosition, true))
	{
		return false;
	}

	FVector WorldOrigin;
	FVector WorldDirection;
	const bool bScreenPositionDeprojected = UGameplayStatics::DeprojectScreenToWorld(this, ScreenPosition, WorldOrigin, WorldDirection);
	if (bScreenPositionDeprojected)
	{
		OutPlanePoint = FMath::LinePlaneIntersection(WorldOrigin, WorldOrigin + WorldDirection * HitResultTraceDistance, InPlaneOrigin, InPlaneNormal);
		return true;
	}

	return false;
}

