import { ButtonBar, Title, returnTrue } from '@cfx-dev/ui-components';
import { observer } from 'mobx-react-lite';
import React from 'react';

import { GameName } from 'cfx/base/game';
import { currentGameNameIs } from 'cfx/base/gameRuntime';
import { AnalyticsLinkButton } from 'cfx/common/parts/AnalyticsLinkButton/AnalyticsLinkButton';
import { useEventHandler } from 'cfx/common/services/analytics/analytics.service';
import { EventActionNames, ElementPlacements } from 'cfx/common/services/analytics/types';
import { $L } from 'cfx/common/services/intl/l10n';

const enum IHomePageNavBarLinkIDs {
  FiveM,
  RedM,
  Forum,
  Portal,
  Discord,
}

interface IHomePageNavBarLink {
  id: IHomePageNavBarLinkIDs;
  href: string;
  label: string;
  visible(): boolean;
}

const homePageNavBarLinks: IHomePageNavBarLink[] = [
  {
    id: IHomePageNavBarLinkIDs.FiveM,
    href: 'https://irfive.ir',
    label: 'IRFive.ir',
    visible: () => currentGameNameIs(GameName.FiveM),
  },
  {
    id: IHomePageNavBarLinkIDs.Forum,
    href: 'https://forum.irfive.ir',
    label: 'Forum',
    visible: returnTrue,
  },
  {
    id: IHomePageNavBarLinkIDs.Discord,
    href: 'https://discord.gg/3DTxk7c3zg',
    label: 'Discord',
    visible: returnTrue,
  },
];

export const HomePageNavBarLinks = observer(function HomePageLinks() {
  const eventHandler = useEventHandler();

  const handleForumClick = React.useCallback(() => {
    const forumItem = homePageNavBarLinks.find(({
      id,
    }) => id === IHomePageNavBarLinkIDs.Forum);

    if (!forumItem) {
      return;
    }

    eventHandler({
      action: EventActionNames.ForumCTA,
      properties: {
        element_placement: ElementPlacements.Nav,
        text: forumItem.label,
        link_url: forumItem.href,
      },
    });
  }, [eventHandler]);

  const linkNodes = homePageNavBarLinks
    .filter(({
      visible,
    }) => visible())
    .map(({
      href,
      label,
      id,
    }) => (
      <Title key={id} title={$L('#Global_OpenLinkInBrowser')}>
        <AnalyticsLinkButton
          to={href}
          text={label}
          size="large"
          theme="transparent"
          elementPlacement={ElementPlacements.Nav}
          onClick={id === IHomePageNavBarLinkIDs.Forum
            ? handleForumClick
            : undefined}
        />
      </Title>
    ));

  return (
    <ButtonBar>{linkNodes}</ButtonBar>
  );
});
